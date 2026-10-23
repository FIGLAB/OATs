/* 4-color 1.54" panel (ESP32-S3-ePaper-1.54G, Jadard JD79660A).
   Full refresh follows Waveshare's EPD_1in54g.c. The optional fast path is
   described in epaper_g_lut.h; only black and white are used either way. */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "epaper_driver_bsp.h"
#include "epaper_g_lut.h"
#include "../../user_config.h"

/* Defaults so the driver builds even without the knobs in user_config.h. */
#ifndef EPD_G_FAST_UPDATE
#define EPD_G_FAST_UPDATE 0
#endif
#ifndef EPD_G_LUT_FORMAT
#define EPD_G_LUT_FORMAT 1
#endif
#ifndef EPD_G_LUT_GROUP_REPEAT
#define EPD_G_LUT_GROUP_REPEAT 0
#endif
#ifndef EPD_G_POLARITY
#define EPD_G_POLARITY 1
#endif
#ifndef EPD_G_WHITE_SCHEME
#define EPD_G_WHITE_SCHEME 1
#endif
#ifndef EPD_G_PLL
#define EPD_G_PLL 0x02
#endif
#ifndef EPD_G_FULL_PLL
#define EPD_G_FULL_PLL 0x08
#endif
#ifndef EPD_G_BLACK_FRAMES
#define EPD_G_BLACK_FRAMES 10
#endif
#ifndef EPD_G_WHITE_FRAMES
#define EPD_G_WHITE_FRAMES 50
#endif
#ifndef EPD_G_CODE_REGS
#define EPD_G_CODE_REGS {0x21, 0x22, 0x23, 0x24}
#endif
#ifndef EPD_G_FAST_MAX_UPDATES
#define EPD_G_FAST_MAX_UPDATES 20
#endif
#ifndef EPD_G_FULL_REFRESH_MIN
#define EPD_G_FULL_REFRESH_MIN 60
#endif

static const char *TAG = "driver_g";

#define G_BLACK      0x0   // 2-bit pixel values of the controller (OTP mode)
#define G_WHITE      0x1
#define G_TO_BLACK   0x2   // transition codes of the fast frame (register-LUT mode)
#define G_TO_WHITE   0x3
#define G_WHITE_BYTE 0x55

static const uint8_t g_btst[7]      = {0x0D, 0x12, 0x30, 0x20, 0x19, 0x2A, 0x22};
static const uint8_t g_code_regs[4] = EPD_G_CODE_REGS;

static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

epaper_panel_t epaper_detect_panel(const custom_lcd_spi_t &cfg)
{
    gpio_config_t io = {};
    io.intr_type    = GPIO_INTR_DISABLE;
    io.mode         = GPIO_MODE_OUTPUT;
    io.pin_bit_mask = (1ULL << cfg.rst);
    io.pull_up_en   = GPIO_PULLUP_ENABLE;
    gpio_config(&io);
    io.mode         = GPIO_MODE_INPUT;
    io.pull_up_en   = GPIO_PULLUP_DISABLE;
    io.pin_bit_mask = (1ULL << cfg.busy);
    gpio_config(&io);

    gpio_set_level((gpio_num_t)cfg.rst, 1); vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)cfg.rst, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)cfg.rst, 1); vTaskDelay(pdMS_TO_TICKS(200));   // let either controller settle

    int high = 0;
    for (int i = 0; i < 8; i++) {
        high += gpio_get_level((gpio_num_t)cfg.busy);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    epaper_panel_t p = (high >= 6) ? EPAPER_PANEL_G : EPAPER_PANEL_BW;
    ESP_LOGI(TAG, "panel probe: BUSY high %d/8 after reset -> %s",
             high, (p == EPAPER_PANEL_G) ? "4-color (G)" : "black/white");
    return p;
}

epaper_driver_display_g::epaper_driver_display_g(int width, int height, custom_lcd_spi_t cfg)
    : epaper_driver_display(width, height, cfg)
{
    g_buffer_len = ((width + 3) / 4) * height;
    if (buffer) heap_caps_free(buffer);           // base allocated the 1-bit size
    buffer = (uint8_t *)heap_caps_malloc(g_buffer_len, MALLOC_CAP_SPIRAM);
    assert(buffer);
    memset(buffer, G_WHITE_BYTE, g_buffer_len);
#if EPD_G_FAST_UPDATE
    prev  = (uint8_t *)heap_caps_malloc(g_buffer_len, MALLOC_CAP_SPIRAM);
    trans = (uint8_t *)heap_caps_malloc(g_buffer_len, MALLOC_CAP_SPIRAM);
    assert(prev && trans);
    ESP_LOGW(TAG, "fast update enabled (experimental): format %d, polarity %d, scheme %d, %d/%d frames",
             EPD_G_LUT_FORMAT, EPD_G_POLARITY, EPD_G_WHITE_SCHEME, EPD_G_BLACK_FRAMES, EPD_G_WHITE_FRAMES);
#endif
}

epaper_driver_display_g::~epaper_driver_display_g()
{
    if (prev)  heap_caps_free(prev);
    if (trans) heap_caps_free(trans);
}

const char *epaper_driver_display_g::name() const
{
    return EPD_G_FAST_UPDATE ? "1.54 4-color (G), fast b/w updates" : "1.54 4-color (G)";
}

bool epaper_driver_display_g::fast_refresh() const { return EPD_G_FAST_UPDATE != 0; }

int epaper_driver_display_g::wait_not_busy(int max_ms)
{
    int busy = lcd_spi_data.busy;
    int64_t t0 = now_ms();
    // BUSY goes LOW while the controller works and HIGH when it is idle.
    // Give it up to 200 ms to assert, then wait for release (bounded).
    for (int i = 0; i < 20 && gpio_get_level((gpio_num_t)busy) == 1; i++) vTaskDelay(pdMS_TO_TICKS(10));
    while (gpio_get_level((gpio_num_t)busy) == 0 && (now_ms() - t0) < max_ms) vTaskDelay(pdMS_TO_TICKS(5));
    return (int)(now_ms() - t0);
}

void epaper_driver_display_g::reset_pulse(int high_ms)
{
    set_rst_1(); vTaskDelay(pdMS_TO_TICKS(high_ms));
    set_rst_0(); vTaskDelay(pdMS_TO_TICKS(2));
    set_rst_1(); vTaskDelay(pdMS_TO_TICKS(high_ms));
}

/* Registers shared by both modes (Waveshare's init, minus power-on). */
void epaper_driver_display_g::common_regs(bool lut_from_register, uint8_t pll, uint8_t cdi)
{
    EPD_SendCommand(0x4D); EPD_SendData(0x78);
    EPD_SendCommand(0x00);                                              // PSR
    EPD_SendData(0x0F);
    EPD_SendData(lut_from_register ? 0xA9 : 0x29);                      // bit7 = LUT_EN
    EPD_SendCommand(0x06);                                              // BTST
    for (int i = 0; i < 7; i++) EPD_SendData(g_btst[i]);
    EPD_SendCommand(0x50); EPD_SendData(cdi);                           // CDI / border
    EPD_SendCommand(0x61);                                              // TRES
    EPD_SendData(Width / 256);  EPD_SendData(Width % 256);
    EPD_SendData(Height / 256); EPD_SendData(Height % 256);
    EPD_SendCommand(0xE9); EPD_SendData(0x01);
    EPD_SendCommand(0x30); EPD_SendData(pll);                           // PLL / frame rate
}

void epaper_driver_display_g::init_sequence(bool fast)
{
    reset_pulse(200);
    common_regs(false, EPD_G_FULL_PLL, 0x37);                           // frame clock: see user_config.h
    EPD_SendCommand(0x04);                                              // power on
    wait_not_busy(2000);
    if (fast) {
        EPD_SendCommand(0xE0); EPD_SendData(0x02);
        EPD_SendCommand(0xE6); EPD_SendData(0x5D);
        EPD_SendCommand(0xA5); EPD_SendData(0x00);
        wait_not_busy(2000);
    }
}

void epaper_driver_display_g::power_off()
{
    EPD_SendCommand(0x02); EPD_SendData(0x00);                          // POWER_OFF
    wait_not_busy(5000);
}

bool epaper_driver_display_g::buffer_has_color() const
{
    for (int i = 0; i < g_buffer_len; i++) {
        // a byte holds 4 pixels; any pixel with bit1 set is yellow or red
        if (buffer[i] & 0xAA) return true;
    }
    return false;
}

/* Factory waveform: the only mode Waveshare documents. 12.4 s of flashing with
   the factory clock, 8.6 s at a fixed 120 Hz (EPD_G_FULL_PLL). */
void epaper_driver_display_g::full_update()
{
    int64_t t0 = now_ms();
    init_sequence(true);
    EPD_SendCommand(0x10);
    writeBytes(buffer, g_buffer_len);
    EPD_SendCommand(0x12); EPD_SendData(0x00);                          // DISPLAY_REFRESH
    wait_not_busy(60000);
    power_off();
    ESP_LOGI(TAG, "full refresh: %lld ms", (long long)(now_ms() - t0));
    if (prev) { memcpy(prev, buffer, g_buffer_len); have_prev = true; }
    fast_count   = 0;
    last_full_ms = now_ms();
}

/* Register-LUT update: only the pixels that changed receive a pulse. */
bool epaper_driver_display_g::fast_update()
{
    int changed = 0;
    for (int i = 0; i < g_buffer_len; i++) {
        uint8_t n = buffer[i], o = prev[i], t = 0;
        for (int k = 0; k < 4; k++) {
            int shift = 6 - 2 * k;
            uint8_t nv = (n >> shift) & 3, ov = (o >> shift) & 3;
            uint8_t code = (nv == ov) ? nv : (nv == G_BLACK ? G_TO_BLACK : G_TO_WHITE);
            if (nv != ov) changed++;
            t |= code << shift;
        }
        trans[i] = t;
    }
    if (changed == 0) {
        ESP_LOGI(TAG, "fast update: nothing changed, panel left alone");
        return true;
    }

    g_fast_cfg_t cfg = {EPD_G_LUT_FORMAT, EPD_G_LUT_GROUP_REPEAT, EPD_G_POLARITY,
                        EPD_G_WHITE_SCHEME, EPD_G_BLACK_FRAMES, EPD_G_WHITE_FRAMES};
    g_phase_t vcom[2], code[4][2];
    g_fast_phases(&cfg, vcom, code);
    uint8_t lut[G_LUT_MAX_BYTES];
    uint8_t vspl = g_fast_vspl_code(&cfg);

    int64_t t0 = now_ms();
    reset_pulse(20);
    wait_not_busy(1000);
    common_regs(true, EPD_G_PLL, 0x97);                                 // LUT from register, border floating
    EPD_SendCommand(0x01);                                              // PWR: internal rails, VGP/VGN +/-20 V,
    EPD_SendData(0x07); EPD_SendData(0x00);                             //      VSPL_0, VSP_1 15 V, VSN_1 -15 V, VSPL_1
    EPD_SendData(vspl); EPD_SendData(0x78); EPD_SendData(0x78); EPD_SendData(vspl);
    EPD_SendCommand(0x04);                                              // power on
    wait_not_busy(2000);

    int n = g_lut_encode(cfg.format, cfg.group_repeat, vcom, 2, lut);
    EPD_SendCommand(0x20); writeBytes(lut, n);                          // VCOM LUT
    for (int c = 0; c < 4; c++) {
        n = g_lut_encode(cfg.format, cfg.group_repeat, code[c], 2, lut);
        EPD_SendCommand(g_code_regs[c]); writeBytes(lut, n);
    }

    EPD_SendCommand(0x10);
    writeBytes(trans, g_buffer_len);
    EPD_SendCommand(0x12); EPD_SendData(0x00);
    int busy_ms = wait_not_busy(30000);
    power_off();
    ESP_LOGI(TAG, "fast update: %d px changed, refresh %d ms, total %lld ms",
             changed, busy_ms, (long long)(now_ms() - t0));
    if (busy_ms > 8000)
        ESP_LOGW(TAG, "that took as long as a factory refresh: the controller probably ignored the register LUT");

    memcpy(prev, buffer, g_buffer_len);
    fast_count++;
    return true;
}

void epaper_driver_display_g::EPD_Init()                 { init_sequence(false); }
void epaper_driver_display_g::EPD_Init_Partial()         { init_sequence(true);  }
void epaper_driver_display_g::EPD_DisplayPartBaseImage() { /* nothing to do */ }
void epaper_driver_display_g::EPD_Clear()                { memset(buffer, G_WHITE_BYTE, g_buffer_len); }
void epaper_driver_display_g::EPD_Display()              { EPD_DisplayPart(); }

void epaper_driver_display_g::EPD_DisplayPart()
{
#if EPD_G_FAST_UPDATE
    bool stale = (now_ms() - last_full_ms) > (int64_t)EPD_G_FULL_REFRESH_MIN * 60 * 1000;
    if (have_prev && fast_count < EPD_G_FAST_MAX_UPDATES && !stale && !buffer_has_color()) {
        fast_update();
        return;
    }
    if (have_prev)
        ESP_LOGI(TAG, "periodic full refresh (%d fast updates, stale=%d)", fast_count, (int)stale);
#endif
    full_update();
}

void epaper_driver_display_g::EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color)
{
    if (x >= Width || y >= Height) return;
    int stride = (Width + 3) / 4;
    uint8_t *b = &buffer[y * stride + (x >> 2)];
    int shift  = 6 - 2 * (x & 3);
    uint8_t v  = (color == DRIVER_COLOR_WHITE) ? G_WHITE : G_BLACK;
    *b = (*b & ~(0x3 << shift)) | (v << shift);
}

void epaper_driver_display_g::EPD_SetBuffer(const uint8_t *data, int len)
{
    // Input is the 1-bit format used by the images and the black/white panel.
    int in_stride  = (Width + 7) / 8;
    int out_stride = (Width + 3) / 4;
    int rows = len / in_stride;
    if (rows > Height) rows = Height;
    memset(buffer, G_WHITE_BYTE, g_buffer_len);
    for (int y = 0; y < rows; y++) {
        const uint8_t *in  = data + y * in_stride;
        uint8_t       *out = buffer + y * out_stride;
        for (int x = 0; x < Width; x++) {
            int white  = (in[x >> 3] >> (7 - (x & 7))) & 1;
            int shift  = 6 - 2 * (x & 3);
            uint8_t v  = white ? G_WHITE : G_BLACK;
            out[x >> 2] = (out[x >> 2] & ~(0x3 << shift)) | (v << shift);
        }
    }
}
