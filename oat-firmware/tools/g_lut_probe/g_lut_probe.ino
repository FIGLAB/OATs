/* g_lut_probe: find out, on the real panel, whether the JD79660A in the
   Waveshare ESP32-S3-ePaper-1.54G accepts a waveform written over SPI
   ("LUT from register"), which byte layout it expects, which LUT register
   drives which pixel code, and which polarity makes black. Then try the
   black/white transition waveform and tune its lengths.

   This is a SEPARATE sketch, not part of oat-firmware: open this .ino in its
   own Arduino IDE window (File > Open), keep the same board settings as
   oat-firmware, Upload, then Tools > Serial Monitor at 115200 baud, line
   ending "New Line". Type a command and press Enter. See README.md.

   Pins are the Waveshare module's; they match user_config.h of oat-firmware. */
#include <SPI.h>
#include "epaper_g_lut.h"

/* Talk on the native USB port AND UART0, whatever "USB CDC On Boot" is set to.
   With CDC enabled, Serial already is the USB port. With it disabled, Serial is
   UART0 (not reachable on this board), so open the USB port ourselves. */
#if ARDUINO_USB_MODE && !ARDUINO_USB_CDC_ON_BOOT
#include "HWCDC.h"
static HWCDC usb_port;      // the core creates no USB serial object in this configuration
#define USB_SER usb_port
#endif
struct DualOut : public Print {
  size_t write(uint8_t c) override {
    Serial.write(c);
#ifdef USB_SER
    USB_SER.write(c);
#endif
    return 1;
  }
  size_t write(const uint8_t *b, size_t n) override {
    Serial.write(b, n);
#ifdef USB_SER
    USB_SER.write(b, n);
#endif
    return n;
  }
};
static DualOut out;
static Stream *inputs[] = { &Serial,
#ifdef USB_SER
  &USB_SER,
#endif
};

#define PIN_DC    10
#define PIN_CS    11
#define PIN_SCK   12
#define PIN_MOSI  13
#define PIN_RST    9
#define PIN_BUSY   8
#define PIN_EPD_PWR 6    // panel rail enable, active LOW
#define PIN_VBAT   17    // battery latch, HIGH keeps the board on

#define W 200
#define H 200
#define FRAME_BYTES ((W / 4) * H)   // 2 bits per pixel

static uint8_t frame[FRAME_BYTES];   // what we send
static uint8_t shown[FRAME_BYTES];   // colours believed to be on the glass (0 black, 1 white)
static const uint8_t btst[7] = {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22};

/* ---- tunables (serial commands change them) ---- */
static g_fast_cfg_t cfg = {1, 0, 1, 1, 10, 50};
static uint8_t pll = 0x02;                       // 50 Hz (factory init uses 0x08: dynamic frame rate)
static uint8_t cdi = 0x97;                       // 0x37 factory border, 0x97 border floating
static bool pwr_override = true;                 // regInit writes PWR (0x01) with our rails
static bool otp_fast_cmds = false;               // regInit also sends Waveshare's E0/E6/A5 after PON
static uint32_t busy_cap_ms = 30000;             // S/N/Y/Z/X give up (and reset) after this
static uint8_t code_regs[4] = {0x21, 0x22, 0x23, 0x24};

/* ---- low level ---- */
static void cmd(uint8_t c) { digitalWrite(PIN_DC, LOW);  digitalWrite(PIN_CS, LOW); SPI.transfer(c); digitalWrite(PIN_CS, HIGH); }
static void dat(uint8_t d) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.transfer(d); digitalWrite(PIN_CS, HIGH); }
static void dats(const uint8_t *b, int n) { digitalWrite(PIN_DC, HIGH); digitalWrite(PIN_CS, LOW); SPI.writeBytes(b, n); digitalWrite(PIN_CS, HIGH); }

/* BUSY is LOW while the controller works. Returns ms waited. */
static uint32_t waitBusy(uint32_t max_ms)
{
  uint32_t t0 = millis();
  while (digitalRead(PIN_BUSY) == HIGH && millis() - t0 < 200) delay(1);     // give it time to assert
  while (digitalRead(PIN_BUSY) == LOW && millis() - t0 < max_ms) delay(2);
  return millis() - t0;
}

static void resetPulse(int ms)
{
  digitalWrite(PIN_RST, HIGH); delay(ms);
  digitalWrite(PIN_RST, LOW);  delay(2);
  digitalWrite(PIN_RST, HIGH); delay(ms);
}

static void commonRegs(bool lutFromRegister, uint8_t pllv, uint8_t cdi)
{
  cmd(0x4D); dat(0x78);
  cmd(0x00); dat(0x0F); dat(lutFromRegister ? 0xA9 : 0x29);   // PSR, bit7 of byte 2 = LUT_EN
  cmd(0x06); for (int i = 0; i < 7; i++) dat(btst[i]);
  cmd(0x50); dat(cdi);
  cmd(0x61); dat(W / 256); dat(W % 256); dat(H / 256); dat(H % 256);
  cmd(0xE9); dat(0x01);
  cmd(0x30); dat(pllv);
}

static void powerOff() { cmd(0x02); dat(0x00); waitBusy(5000); }

/* ---- SPI read-back. The module wires only SDA, so reads are bit-banged on
   the same line: command out, then the controller drives SDA (datasheet 7.2,
   read mode). The hardware SPI is detached while we do it. ---- */
static void spiDetach()
{
  SPI.endTransaction(); SPI.end();
  pinMode(PIN_SCK, OUTPUT);  digitalWrite(PIN_SCK, LOW);
  pinMode(PIN_MOSI, OUTPUT); digitalWrite(PIN_MOSI, LOW);
}
static void spiAttach()
{
  SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
}
static void bbWrite(uint8_t b)
{
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (b >> i) & 1); delayMicroseconds(1);
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(1);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(1);
  }
}
static uint8_t bbRead()
{
  uint8_t b = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_SCK, HIGH); delayMicroseconds(1);
    b = (b << 1) | (digitalRead(PIN_MOSI) & 1);
    digitalWrite(PIN_SCK, LOW);  delayMicroseconds(1);
  }
  return b;
}
/* Send a read command, then clock n bytes back. CS stays low throughout. */
static void readRegs(uint8_t reg, uint8_t *outb, int n)
{
  spiDetach();
  digitalWrite(PIN_DC, LOW); digitalWrite(PIN_CS, LOW);
  bbWrite(reg);
  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_MOSI, INPUT);          // SDA is now driven by the controller
  delayMicroseconds(5);
  for (int i = 0; i < n; i++) outb[i] = bbRead();
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_MOSI, OUTPUT);
  spiAttach();
}

static uint8_t mtp[4096];            // copy of the controller's MTP (factory waveform + settings)
static int mtp_len = 0, mtp_start = 0;

/* Same test as epaper_detect_panel() in oat-firmware: after a reset the
   JD79660A (G board) holds BUSY high, the SSD1681 (B/W board) holds it low. */
static bool panel_is_g = false;
static void detectPanel()
{
  resetPulse(20);
  delay(200);
  int high = 0;
  for (int i = 0; i < 8; i++) { high += digitalRead(PIN_BUSY); delay(10); }
  panel_is_g = high >= 6;
  out.printf("panel check: BUSY high %d/8 after reset -> %s\n", high,
             panel_is_g ? "4-color G board (JD79660A), OK" : "black/white board (SSD1681): this probe is for the G board only");
}

/* Waveshare's factory ("OTP") fast init. */
static void otpInit()
{
  resetPulse(200);
  commonRegs(false, 0x08, 0x37);
  cmd(0x04); waitBusy(2000);
  cmd(0xE0); dat(0x02); cmd(0xE6); dat(0x5D); cmd(0xA5); dat(0x00);
  waitBusy(2000);
}

/* Our init: reset, our rails, our frame clock, border floating, LUT_EN as asked. */
static void regInit(bool lutEn = true, bool pon = true)
{
  resetPulse(20);
  waitBusy(1000);
  commonRegs(lutEn, pll, cdi);
  if (pwr_override) {
    uint8_t vspl = g_fast_vspl_code(&cfg);
    cmd(0x01); dat(0x07); dat(0x00); dat(vspl); dat(0x78); dat(0x78); dat(vspl);   // PWR
  }
  if (!pon) return;                       // "I x n": stop here, send PON yourself with o
  cmd(0x04); waitBusy(2000);
  if (otp_fast_cmds) { cmd(0xE0); dat(0x02); cmd(0xE6); dat(0x5D); cmd(0xA5); dat(0x00); waitBusy(2000); }
}

static void sendLut(uint8_t reg, const g_phase_t *ph, int n)
{
  uint8_t lut[G_LUT_MAX_BYTES];
  int len = g_lut_encode(cfg.format, cfg.group_repeat, ph, n, lut);
  if (len == 0) { out.println("!! LUT does not fit"); len = g_lut_bytes(cfg.format); memset(lut, 0, len); }
  cmd(reg); dats(lut, len);
}

static void sendZeroLut(uint8_t reg)
{
  uint8_t lut[G_LUT_MAX_BYTES] = {0};
  cmd(reg); dats(lut, g_lut_bytes(cfg.format));
}

static uint32_t sendFrameAndRefresh(const uint8_t *f, uint32_t max_ms)
{
  cmd(0x10); dats(f, FRAME_BYTES);
  cmd(0x12); dat(0x00);
  uint32_t t = waitBusy(max_ms);
  if (digitalRead(PIN_BUSY) == LOW) {          // still running: stop it rather than leave the panel driven
    out.printf("!! BUSY still low after %lu ms: resetting the controller\n", (unsigned long)t);
    resetPulse(20); waitBusy(1000);
    return t;
  }
  powerOff();
  return t;
}

/* ---- frame helpers ---- */
static void setPx(uint8_t *f, int x, int y, uint8_t code)
{
  uint8_t *b = &f[y * (W / 4) + (x >> 2)];
  int shift = 6 - 2 * (x & 3);
  *b = (*b & ~(0x3 << shift)) | ((code & 3) << shift);
}
static uint8_t getPx(const uint8_t *f, int x, int y)
{
  return (f[y * (W / 4) + (x >> 2)] >> (6 - 2 * (x & 3))) & 3;
}

/* ---- commands ---- */
static void cmdF()
{
  // Known starting picture with the factory waveform: left half black, right half white.
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(shown, x, y, x < W / 2 ? 0 : 1);
  memcpy(frame, shown, FRAME_BYTES);
  out.println("F: factory refresh, left black / right white (expect ~15 s of flashing)");
  otpInit();
  uint32_t t = sendFrameAndRefresh(frame, 60000);
  out.printf("F: done, BUSY %lu ms\n", (unsigned long)t);
}

static void cmdZ()
{
  out.println("Z: LUT_EN=1, all-zero bytes written to 0x20 and 0x21..0x24, picture unchanged");
  regInit(true);
  sendZeroLut(0x20);
  for (int c = 0; c < 4; c++) sendZeroLut(code_regs[c]);
  memcpy(frame, shown, FRAME_BYTES);
  uint32_t t = sendFrameAndRefresh(frame, busy_cap_ms);
  out.printf("Z: BUSY %lu ms\n", (unsigned long)t);
}

/* Controls: our init with LUT_EN 0 (N) or 1 (Y), nothing written to any LUT register. */
static void cmdNY(bool lutEn)
{
  char id = lutEn ? 'Y' : 'N';
  out.printf("%c: LUT_EN=%d, our init, no LUT written, picture unchanged\n", id, lutEn ? 1 : 0);
  regInit(lutEn);
  memcpy(frame, shown, FRAME_BYTES);
  uint32_t t = sendFrameAndRefresh(frame, busy_cap_ms);
  out.printf("%c: BUSY %lu ms\n", id, (unsigned long)t);
}

/* Read-back check: revision registers and the temperature sensor. */
static void cmdR()
{
  uint8_t b[4];
  resetPulse(20); waitBusy(1000);
  readRegs(0x70, b, 3); out.printf("R: REV  (0x70) = %02X %02X %02X   (datasheet says 08 02 01)\n", b[0], b[1], b[2]);
  readRegs(0x9E, b, 4); out.printf("R: REV2 (0x9E) = %02X %02X %02X %02X\n", b[0], b[1], b[2], b[3]);
  readRegs(0x43, b, 2); out.printf("R: TSR  (0x43) = %02X %02X   (temperature sensor)\n", b[0], b[1]);
  out.println("R: all FF or all 00 means the read path does not work");
}

/* Dump the MTP (factory waveform 0x000..0xEFF, settings 0xF00..0xFFF) with RMTP.
   PGM_CFG only sets the read window; nothing is ever programmed (no 0x91). */
static void cmdM(int start, int len)
{
  if (start < 0) start = 0;
  if (len < 1) len = 1;
  if (start + len > 4096) len = 4096 - start;
  resetPulse(20); waitBusy(1000);
  cmd(0x4D); dat(0x78);
  cmd(0xA2); dat(0x00); dat(start >> 8); dat(start & 0xFF); dat(len >> 8); dat(len & 0xFF);
  spiDetach();
  digitalWrite(PIN_DC, LOW); digitalWrite(PIN_CS, LOW);
  bbWrite(0x92);
  digitalWrite(PIN_DC, HIGH);
  pinMode(PIN_MOSI, INPUT);
  delayMicroseconds(5);
  uint8_t dummy = bbRead();
  for (int i = 0; i < len; i++) mtp[i] = bbRead();
  digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_MOSI, OUTPUT);
  spiAttach();
  mtp_len = len; mtp_start = start;
  int ff = 0, zz = 0;
  for (int i = 0; i < len; i++) { if (mtp[i] == 0xFF) ff++; if (mtp[i] == 0x00) zz++; }
  out.printf("M: RMTP start 0x%03X len %d, dummy byte %02X, %d bytes FF, %d bytes 00\n", start, len, dummy, ff, zz);
  for (int i = 0; i < len; i += 32) {
    out.printf("MTP %03X:", start + i);
    for (int j = i; j < i + 32 && j < len; j++) out.printf(" %02X", mtp[j]);
    out.println();
  }
  out.println("M: end");
}

/* Write n bytes of the MTP dump, starting at offset off, to command reg (default 0x20).
   No init and no refresh here: use I before and S after. */
static void cmdL(int n, int off, int reg)
{
  if (mtp_start != 0 || mtp_len < off + n || n < 1) { out.println("L: run M 0 4096 first / range"); return; }
  cmd((uint8_t)reg); dats(mtp + off, n);
  out.printf("L: MTP[0x%03X..0x%03X] (%d bytes) written to 0x%02X\n", off, off + n - 1, n, reg);
}

/* Program-mode RAM test: enter program mode (0x90), load a 4096-byte image with
   the data command, then hardware-reset to leave program mode. 0x91 (Active
   Program) is never sent, so the MTP itself cannot change. 'z' loads zeros,
   'm' the MTP copy, 'f' 0xFF. Afterwards use I 1 and S to see if the engine
   picks the RAM image up. */
static void cmdW(char what)
{
  static uint8_t img[4096];
  if (what == 'm') { if (mtp_len < 4096) { out.println("W m: run M 0 4096 first"); return; } memcpy(img, mtp, 4096); }
  else memset(img, what == 'f' ? 0xFF : 0x00, 4096);
  resetPulse(20); waitBusy(1000);
  cmd(0x4D); dat(0x78);
  cmd(0xA2); dat(0x00); dat(0x00); dat(0x00); dat(0x10); dat(0x00);   // window 0x000, size 0x1000
  cmd(0x90);                                                        // program mode
  cmd(0x10); dats(img, 4096);
  delay(10);
  uint32_t t = waitBusy(2000);
  out.printf("W %c: 4096 bytes written in program mode (BUSY wait %lu ms); leaving program mode by reset\n", what, (unsigned long)t);
  resetPulse(20); waitBusy(1000);
}

/* Raw primitives, so experiments can be driven over serial without reflashing. */
static void cmdC(const char *a)
{
  int v[256], n = 0;
  char *e;
  for (const char *q = a; n < 256; q = e) {
    long x = strtol(q, &e, 16);
    if (e == q) break;
    v[n++] = (int)x;
  }
  if (n < 1) { out.println("C cmd [data...]  (hex bytes)"); return; }
  cmd((uint8_t)v[0]);
  for (int i = 1; i < n; i++) dat((uint8_t)v[i]);
  out.printf("C: sent 0x%02X + %d data bytes\n", v[0], n - 1);
}

static void cmdS(char pat)
{
  switch (pat) {
    case 'b': for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(frame, x, y, x / 50); memcpy(shown, frame, FRAME_BYTES); break;
    case 'i': for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(frame, x, y, getPx(shown, x, y) == 0 ? 3 : 2);
              for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(shown, x, y, getPx(shown, x, y) == 0 ? 1 : 0); break;
    case '0': case '1': case '2': case '3':
              for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(frame, x, y, pat - '0'); memcpy(shown, frame, FRAME_BYTES); break;
    default:  memcpy(frame, shown, FRAME_BYTES); pat = 's'; break;
  }
  uint32_t t = sendFrameAndRefresh(frame, busy_cap_ms);
  out.printf("S: pattern %c sent with the current init, BUSY %lu ms\n", pat, (unsigned long)t);
}

static void cmdP(int reg, int lvl, int frames)
{
  // One pulse on one LUT register, four vertical bands carrying codes 0..3.
  out.printf("P: reg 0x%02X level %d (%s) for %d frames; bands x0-49=code0 50-99=code1 100-149=code2 150-199=code3\n",
                reg, lvl, lvl == 1 ? "VSP" : lvl == 2 ? "VSN" : lvl == 3 ? "VSPL" : "VCOM", frames);
  regInit(true);
  g_phase_t vc[1] = {{G_LVL_VCOM, (uint16_t)frames}};
  g_phase_t px[1] = {{(uint8_t)lvl, (uint16_t)frames}};
  sendLut(0x20, vc, 1);
  for (int c = 0; c < 4; c++) {
    if (code_regs[c] == reg) sendLut(reg, px, 1); else sendZeroLut(code_regs[c]);
  }
  if (reg != code_regs[0] && reg != code_regs[1] && reg != code_regs[2] && reg != code_regs[3]) sendLut(reg, px, 1);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(frame, x, y, x / 50);
  uint32_t t = sendFrameAndRefresh(frame, busy_cap_ms);
  out.printf("P: BUSY %lu ms. Which band changed, and to which colour?\n", (unsigned long)t);
  out.println("   (the picture on the glass is now unknown: run F before X)");
}

static void cmdX()
{
  // Invert the picture with the transition waveform: black -> white, white -> black.
  regInit(true);
  g_phase_t vcom[2], code[4][2];
  g_fast_phases(&cfg, vcom, code);
  sendLut(0x20, vcom, 2);
  for (int c = 0; c < 4; c++) sendLut(code_regs[c], code[c], 2);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(frame, x, y, getPx(shown, x, y) == 0 ? 3 : 2);
  uint32_t t = sendFrameAndRefresh(frame, busy_cap_ms);
  for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) setPx(shown, x, y, getPx(shown, x, y) == 0 ? 1 : 0);
  out.printf("X: inverted with %d black + %d white frames, BUSY %lu ms (now expect left %s / right %s)\n",
                cfg.black_frames, cfg.white_frames, (unsigned long)t,
                getPx(shown, 0, 0) ? "white" : "black", getPx(shown, W - 1, 0) ? "white" : "black");
}

static void printSettings()
{
  out.printf("init: pll 0x%02X  cdi 0x%02X  pwr_override %d  otp_fast_cmds %d  busy cap %lu ms\n", pll, cdi, pwr_override, otp_fast_cmds, (unsigned long)busy_cap_ms);
  out.printf("format %d  group_repeat %d  polarity %d  white_scheme %d  black %d fr  white %d fr  regs %02X %02X %02X %02X  VSPL code 0x%02X\n",
                cfg.format, cfg.group_repeat, cfg.polarity, cfg.white_scheme, cfg.black_frames, cfg.white_frames,
                code_regs[0], code_regs[1], code_regs[2], code_regs[3], g_fast_vspl_code(&cfg));
  out.println("for user_config.h:");
  out.printf("  #define EPD_G_LUT_FORMAT %d / EPD_G_LUT_GROUP_REPEAT %d / EPD_G_CODE_REGS {0x%02X,0x%02X,0x%02X,0x%02X} / EPD_G_POLARITY %d / EPD_G_WHITE_SCHEME %d / EPD_G_PLL 0x%02X / EPD_G_BLACK_FRAMES %d / EPD_G_WHITE_FRAMES %d\n",
                cfg.format, cfg.group_repeat, code_regs[0], code_regs[1], code_regs[2], code_regs[3],
                cfg.polarity, cfg.white_scheme, pll, cfg.black_frames, cfg.white_frames);
}

static void help()
{
  out.println(
    "g_lut_probe commands (type a line, Enter):\n"
    "  F              factory full refresh: left black / right white (always run this first, and to clean up)\n"
    "  N / Y          control refreshes with our init and LUT_EN 0 / 1, nothing written to LUT registers\n"
    "  Z              LUT_EN=1 with all-zero bytes in 0x20 and 0x21..0x24\n"
    "  R              read REV/REV2/temperature back over SPI (checks the read path)\n"
    "  M [start len]  dump the controller MTP (factory waveform) as hex; default 0 4096\n"
    "  L n [off reg]  write n MTP bytes from offset off (dec) to command reg (hex, default 20); no init, no refresh\n"
    "  t ms           BUSY cap for the experimental refreshes (default 30000)\n"
    "  I 0|1 [n]      our init with LUT_EN 0|1 (n: stop before PON)   C hh [hh..]  raw command + data (hex)\n"
    "  o              PON (0x04) + BUSY wait        B            wait for BUSY (cap t ms), reset if still low\n"
    "  W z|m|f        program-mode RAM test: load zeros / MTP copy / FF via 0x10 after 0x90, then reset (never 0x91)\n"
    "  S [s|b|i|0-3]  send frame + refresh: s shown, b bands, i inverted codes 2/3, 0-3 solid code\n"
    "  P reg lvl n    single pulse: LUT register reg (hex, 21..24), level lvl (1 VSP, 2 VSN, 3 VSPL), n frames; bands show codes 0..3\n"
    "  X              invert the picture with the transition waveform (repeat to judge speed, contrast, ghosting)\n"
    "  T n            run X n times, 1 s apart\n"
    "  f 1|2          LUT byte layout      g 0|1   group repeat byte (format 1)\n"
    "  b 1|2          black polarity       s 1|2   white scheme (polarity 1)\n"
    "  k n            black frames         w n     white frames\n"
    "  p hex          PLL for our init (02 = 50 Hz fixed, 08 = factory dynamic)   c hex   CDI (37 factory, 97 border floating)\n"
    "  v 0|1          our init writes PWR with our rails   e 0|1   our init also sends Waveshare's E0/E6/A5\n"
    "  m r0 r1 r2 r3  LUT registers (hex) for codes 0..3\n"
    "  ?              print settings       d       re-run the panel check\n"
    "  h              this help");
}

static bool got_command = false;

static void handle(String line)
{
  line.trim();
  if (line.length() == 0) return;
  got_command = true;
  out.printf("> %s\n", line.c_str());
  char c = line[0];
  if (!panel_is_g && strchr("FZPXTNYRMLICSWoB", c)) {
    out.println("refused: no G panel detected (type d to re-check, or h for help)");
    return;
  }
  const char *a = line.c_str() + 1;
  int v1 = 0, v2 = 0, v3 = 0, v4 = 0;
  switch (c) {
    case 'F': cmdF(); break;
    case 'Z': cmdZ(); break;
    case 'N': cmdNY(false); break;
    case 'Y': cmdNY(true); break;
    case 'R': cmdR(); break;
    case 'M': { int st = 0, ln = 4096; sscanf(a, "%d %d", &st, &ln); cmdM(st, ln); } break;
    case 'L': { int n = 3840, off = 0, reg = 0x20; sscanf(a, "%d %d %x", &n, &off, &reg); cmdL(n, off, reg); } break;
    case 't': if (sscanf(a, "%d", &v1) == 1) busy_cap_ms = v1; printSettings(); break;
    case 'I': { char np = 0; int k = sscanf(a, "%d %c", &v1, &np);
                if (k >= 1) { bool pon = !(k == 2 && (np == 'n' || np == 'N')); regInit(v1 != 0, pon);
                              out.printf("I: init done, LUT_EN=%d%s\n", v1 != 0, pon ? "" : ", no PON yet"); }
                else out.println("I 0|1 [n]"); } break;
    case 'o': { cmd(0x04); uint32_t t = waitBusy(2000); out.printf("o: PON sent, BUSY %lu ms\n", (unsigned long)t); } break;
    case 'B': { uint32_t t = waitBusy(busy_cap_ms);
                if (digitalRead(PIN_BUSY) == LOW) { out.printf("!! BUSY still low after %lu ms: resetting the controller\n", (unsigned long)t); resetPulse(20); waitBusy(1000); }
                else out.printf("B: BUSY high after %lu ms\n", (unsigned long)t); } break;
    case 'C': cmdC(a); break;
    case 'W': { char w = 'z'; sscanf(a, " %c", &w); cmdW(w); } break;
    case 'S': { char pat = 's'; sscanf(a, " %c", &pat); cmdS(pat); } break;
    case 'P': if (sscanf(a, "%x %d %d", &v1, &v2, &v3) == 3) cmdP(v1, v2, v3); else out.println("P reg lvl n"); break;
    case 'X': cmdX(); break;
    case 'T': if (sscanf(a, "%d", &v1) == 1) for (int i = 0; i < v1; i++) { cmdX(); delay(1000); } break;
    case 'f': if (sscanf(a, "%d", &v1) == 1) cfg.format = v1 == 2 ? 2 : 1; printSettings(); break;
    case 'g': if (sscanf(a, "%d", &v1) == 1) cfg.group_repeat = v1; printSettings(); break;
    case 'b': if (sscanf(a, "%d", &v1) == 1) cfg.polarity = v1 == 2 ? 2 : 1; printSettings(); break;
    case 's': if (sscanf(a, "%d", &v1) == 1) cfg.white_scheme = v1 == 2 ? 2 : 1; printSettings(); break;
    case 'k': if (sscanf(a, "%d", &v1) == 1) cfg.black_frames = v1; printSettings(); break;
    case 'w': if (sscanf(a, "%d", &v1) == 1) cfg.white_frames = v1; printSettings(); break;
    case 'p': if (sscanf(a, "%x", &v1) == 1) pll = v1; printSettings(); break;
    case 'c': if (sscanf(a, "%x", &v1) == 1) cdi = v1; printSettings(); break;
    case 'v': if (sscanf(a, "%d", &v1) == 1) pwr_override = v1 != 0; printSettings(); break;
    case 'e': if (sscanf(a, "%d", &v1) == 1) otp_fast_cmds = v1 != 0; printSettings(); break;
    case 'm': if (sscanf(a, "%x %x %x %x", &v1, &v2, &v3, &v4) == 4) { code_regs[0] = v1; code_regs[1] = v2; code_regs[2] = v3; code_regs[3] = v4; } printSettings(); break;
    case '?': printSettings(); break;
    case 'd': detectPanel(); break;
    default:  help(); break;
  }
  out.println("## done");    // end-of-command marker for scripts driving the probe
}

void setup()
{
  pinMode(PIN_VBAT, OUTPUT);    digitalWrite(PIN_VBAT, HIGH);     // keep the board on if on battery
  pinMode(PIN_EPD_PWR, OUTPUT); digitalWrite(PIN_EPD_PWR, LOW);   // panel rail on
  pinMode(PIN_DC, OUTPUT); pinMode(PIN_CS, OUTPUT); pinMode(PIN_RST, OUTPUT); pinMode(PIN_BUSY, INPUT);
  digitalWrite(PIN_CS, HIGH); digitalWrite(PIN_RST, HIGH);
  SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);
  SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0));
  Serial.begin(115200);
#ifdef USB_SER
  USB_SER.begin();
#endif
  delay(1500);
  memset(shown, 0x55, FRAME_BYTES);   // assume white until F runs
  out.println("\ng_lut_probe for ESP32-S3-ePaper-1.54G (JD79660A)");
  detectPanel();
  help();
  printSettings();
}

void loop()
{
  for (Stream *s : inputs) if (s->available()) handle(s->readStringUntil('\n'));
  static uint32_t last_hint = 0;
  if (!got_command && millis() - last_hint > 5000) {   // the monitor usually opens after the banner
    last_hint = millis();
    out.printf("g_lut_probe ready (%s). Type h and press Enter for help.\n",
               panel_is_g ? "G panel found" : "NO G panel found, type d to re-check");
  }
}
