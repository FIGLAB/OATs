# g_lut_probe — fast black/white updates on the 1.54" G (4-color) panel

The JD79660A controller on the Waveshare ESP32-S3-ePaper-1.54G only documents
its factory waveform (about 15 s of flashing per update). Its datasheet also
has a "LUT from register" mode bit and shows the VCOM LUT being written with
command 0x20, but not the byte layout. This sketch finds the missing facts on
the real panel so that `EPD_G_FAST_UPDATE` in `oat-firmware/user_config.h`
can be switched on with values that are known to work.

`epaper_g_lut.h` here is a copy of `oat-firmware/src/display/epaper_g_lut.h`;
keep them identical.

The probe never sends 0x91 (Active Program) and never applies VMTP, so it
cannot alter the controller's MTP. 0xA2 is used only to set the read window.

## Flash and connect

This is a separate sketch. Typing its commands into the serial monitor while
oat-firmware is running does nothing: that firmware only knows `send` and
`emotion`.

1. In the Arduino IDE: File > Open > `oat-firmware/tools/g_lut_probe/g_lut_probe.ino`.
   It opens in its own window.
2. Tools menu: same board and options as oat-firmware (ESP32S3 Dev Module,
   USB CDC On Boot: Enabled, PSRAM: OPI, Partition Scheme: Huge APP). The
   sketch also talks on the USB port when USB CDC On Boot is Disabled.
3. Upload to the **1.54G** board.
4. Tools > Serial Monitor, 115200 baud, line ending "New Line". Every five
   seconds the board prints `g_lut_probe ready (G panel found)` until the first
   command. It also reports which panel it found; on the black/white board the
   panel commands are refused.
5. Type a command in the monitor's input field and press Enter. The board
   echoes the line (`> F`) and then reports. Type `h` for the command list.

To go back to the normal firmware afterwards, upload oat-firmware again.

## Hardware findings (2026-10-09, driven from `scripts/probe_cmd.py`)

Measured on the 1.54G board, BUSY time of one full refresh of an unchanged
picture:

| init | BUSY |
|---|---|
| Waveshare fast init (`F`) | 12.35 s |
| same without E0/E6/A5 (normal mode) | 19.8 s |
| normal mode, PLL 0x02 (50 Hz fixed) | 25.6 s |
| fast mode, PLL 0x07 (120 Hz fixed) | 8.6 s |
| fast mode, PLL 0x06 / 05 / 04 / 03 | 10.4 / 12.3 / 14.0 / 16.1 s |
| fast mode, PLL 0x17 / 27 / 47 / 87 / F7 (upper bits) | 8.6 s, no change |
| normal mode (room temperature), PLL 0x07 | 10.5 s |
| E0 02, E6 00 (0 degC), A5 00 | > 45 s |
| E0 02, E6 19 (25 degC), A5 00 | 19.8 s |
| E0 02, E6 3C (60 degC), A5 00 | 13.8 s |
| E0 02, E6 7F, A5 00 | 12.35 s |

So Waveshare's "fast" commands are a temperature override: 0xE0 bit1
enables it, 0xE6 is the temperature, 0xA5 0x00 applies it. The MTP header
holds the zone breakpoints 2, 5, 10, 15, 20, 25, 30, 60 degC; the hottest
zone is the shortest factory waveform. The waveform is counted in frames,
so a fixed 120 Hz frame clock (PLL 0x07) shortens it further. The user
judged the 120 Hz picture (black/white halves, four colour bands) normal.
The upper bits of the PLL register do nothing, so 8.6 s is the floor for
the stored waveform. The firmware uses this: `EPD_G_FULL_PLL` in
`user_config.h` (default 0x07).

Register path: the mode bit (LUT_EN) alone changes nothing. Any write of
8 or more bytes to 0x20, with any content and with LUT_EN 0 or 1, makes the
refresh never finish (the probe resets after the cap). 1 to 7 bytes have no
visible effect on timing. During that 0x20 hang the stored waveform still
runs (the usual flashing, then the panel sits with BUSY low).

0xE0 bit0 (`C E0 01`, then `C A5 00`) is different: the refresh never
finishes either, but the panel stays completely still from the start, so
this bit really does switch the waveform source away from the MTP. What it
switches to is still unknown: with the bit set, non-zero pulse tables in
0x20..0x24 (42/44/56 bytes, JD79653A-style groups), the factory data in
0x20, 0x21 or 0x13 (DTM2), the data in program-mode RAM (0x90 + 0x10,
no reset), tables written before or after 0xA5 0x00, 0xA5 0x01, 0xA5
carrying the table, 0xE0 0x03 with 0xE6, LUT_EN=1 on top, and the same
tables written before power-on (`I 0 n` ... `o`) all leave the panel still
and BUSY low. Commands 0x21 to 0x2F are ignored without the bit. The factory data written back through
0x20 in every framing tried (full 3840 or 4096 bytes, header skipped) also
never finishes, so the register table is not the MTP's compressed format.
Writing the MTP area in program mode (0x90 then 0x10, never 0x91) changes
nothing. Read-back works: REV = 88 02 01, two MTP dumps identical
(`mtp_dump_1p54G.txt`, `.bin`).

## Protocol

Always start with `F`. It draws a known picture with the factory waveform:
left half black, right half white. Expect the usual 15 s of flashing.

1. **Controls.** `N` refreshes with our init and LUT_EN=0, `Y` with LUT_EN=1,
   nothing written to any LUT register. Compare both with `Z`. If all three
   look and time the same, the written bytes change nothing. If `Y` or `Z`
   differ from `N`, the bit and the 0x20 bytes matter.

2. **Read path.** `R` reads the revision registers back; the datasheet value
   is `08 02 01`. All FF or all 00 means read-back does not work on this
   wiring and the MTP route is closed.

3. **Factory waveform.** `M` dumps the 4096-byte MTP as hex. Save the output.
   `L` writes the waveform part (3840 bytes) back into 0x20 with LUT_EN=1
   and refreshes. If that reproduces the factory refresh, the register path
   is confirmed and the format is whatever the dump shows. `L n` with smaller
   n truncates the waveform and shows how the chip reacts to a short table.

4. **Raw experiments.** `I 1` runs our init with LUT_EN=1 (`I 1 n` stops
   before power-on; `o` sends power-on later), `C 20 hh hh ...`
   writes arbitrary bytes to a command, `B` waits for BUSY after a raw
   `C 12 00`, `S` sends the frame and refreshes
   (`S i` inverts the picture using codes 2/3, `S b` draws four bands,
   `S 0`..`S 3` draw a solid code). These are meant to be driven from a
   script over the serial port.

5. The older per-register commands (`P`, `X`, `T`, `f g b s k w p m`) assume
   the JD79653A-style tables in 0x21..0x24. Keep them only if step 1 shows
   those registers do something.

## Safety

Nothing here exceeds the controller's documented voltage ranges. Keep pulses
short (the sketch caps a slot at 63 or 255 frames), do not leave a pixel under
one polarity for many updates, and finish every session with `F`, which
restores the panel with the factory waveform. The white pulse is five times
longer than the black one so that a black/white round trip is DC balanced.
