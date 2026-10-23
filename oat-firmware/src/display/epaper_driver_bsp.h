#ifndef EPAPER_DRIVER_BSP_H
#define EPAPER_DRIVER_BSP_H

#include <stdint.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"

/* Display color */
typedef enum {
    DRIVER_COLOR_WHITE  = 0xff,
    DRIVER_COLOR_BLACK  = 0x00,
    FONT_BACKGROUND = DRIVER_COLOR_WHITE,
}COLOR_IMAGE;

typedef struct {
    uint8_t cs;
    uint8_t dc;
    uint8_t rst;
    uint8_t busy;
    uint8_t mosi;
    uint8_t scl;
    int spi_host;
    int buffer_len;
}custom_lcd_spi_t;

/* Which 1.54" panel is fitted. The two Waveshare boards share every pin but
   use different controllers with incompatible command sets. */
typedef enum {
    EPAPER_PANEL_BW = 1,   // ESP32-S3-ePaper-1.54:  black/white, SSD1681, partial refresh
    EPAPER_PANEL_G  = 2,   // ESP32-S3-ePaper-1.54G: black/white/red/yellow, full refresh only
} epaper_panel_t;

/* Probe the fitted panel. After a reset pulse the SSD1681 idles with BUSY
   low, while the 4-color controller idles with BUSY high. The panel must be
   powered when this is called. */
epaper_panel_t epaper_detect_panel(const custom_lcd_spi_t &cfg);

/* Black/white panel driver (SSD1681). Also the base class for the 4-color
   driver: the app only uses the public methods, so either panel plugs in. */
class epaper_driver_display {
protected:
    const custom_lcd_spi_t lcd_spi_data;
    const int Width;
    const int Height;
    spi_device_handle_t spi;
    uint8_t *buffer = NULL;

    void spi_gpio_init();
    void spi_port_init();
    void read_busy();

    void set_cs_1(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,1);}
    void set_cs_0(){gpio_set_level((gpio_num_t)lcd_spi_data.cs,0);}
    void set_dc_1(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,1);}
    void set_dc_0(){gpio_set_level((gpio_num_t)lcd_spi_data.dc,0);}
    void set_rst_1(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,1);}
    void set_rst_0(){gpio_set_level((gpio_num_t)lcd_spi_data.rst,0);}

    void SPI_SendByte(uint8_t data);
    void EPD_SendData(uint8_t data);
    void EPD_SendCommand(uint8_t command);
    void writeBytes(uint8_t *buffer,int len);
    void writeBytes(const uint8_t *buffer, int len);
    void EPD_SetWindows(uint16_t Xstart, uint16_t Ystart, uint16_t Xend, uint16_t Yend);
    void EPD_SetCursor(uint16_t Xstart, uint16_t Ystart);
    void EPD_SetLut(const uint8_t *lut);
    void EPD_TurnOnDisplay();
    void EPD_TurnOnDisplayPart();

public:
    epaper_driver_display(int width, int height,custom_lcd_spi_t _lcd_spi_data);
    virtual ~epaper_driver_display();

    virtual const char *name() const { return "1.54 black/white (SSD1681)"; }
    /* True when an update takes well under a second (partial mode). The
       emotion animation and the periodic "keep lively" refresh rely on it. */
    virtual bool fast_refresh() const { return true; }

    virtual void EPD_Init();    /* 墨水屏初始化 */
    virtual void EPD_Clear();   /* 清空屏幕 */
    virtual void EPD_Display(); /* 刷buffer到墨水屏 */

    /*局部刷新*/
    virtual void EPD_DisplayPartBaseImage();
    virtual void EPD_Init_Partial();
    virtual void EPD_DisplayPart();
    virtual void EPD_DrawColorPixel(uint16_t x, uint16_t y,uint8_t color);
    /* Load a 1-bit image: MSB first, bit 1 = white, (Width+7)/8 bytes per row. */
    virtual void EPD_SetBuffer(const uint8_t *data, int len);
};

/* 4-color panel driver (ESP32-S3-ePaper-1.54G, Jadard JD79660A). Takes the
   same 1-bit images as the black/white driver; red and yellow are not used.

   Normal mode: every update is the factory full refresh (about 15 s of
   flashing) and re-initialises the panel first, so it survives the app
   power-cycling the panel between updates.

   Experimental fast mode (EPD_G_FAST_UPDATE in user_config.h): the driver
   keeps the last frame it showed, uploads a frame of transition codes
   (stays / to black / to white) and runs a short waveform of its own through
   the controller's "LUT from register" mode. A factory full refresh is still
   run at boot and periodically to clear ghosting. See epaper_g_lut.h and
   tools/g_lut_probe. */
class epaper_driver_display_g : public epaper_driver_display {
private:
    int      g_buffer_len;        // ((Width+3)/4) * Height: 2 bits per pixel
    uint8_t *prev  = NULL;        // frame currently on the glass (fast mode)
    uint8_t *trans = NULL;        // scratch: transition codes (fast mode)
    bool     have_prev   = false;
    int      fast_count  = 0;     // fast updates since the last full refresh
    int64_t  last_full_ms = 0;

    int  wait_not_busy(int max_ms = 60000);   // BUSY is active LOW; returns ms waited
    void reset_pulse(int high_ms);
    void common_regs(bool lut_from_register, uint8_t pll, uint8_t cdi);
    void init_sequence(bool fast);
    void power_off();
    void full_update();
    bool fast_update();
    bool buffer_has_color() const;
public:
    epaper_driver_display_g(int width, int height, custom_lcd_spi_t _lcd_spi_data);
    ~epaper_driver_display_g() override;

    const char *name() const override;
    bool fast_refresh() const override;       // true only with EPD_G_FAST_UPDATE

    void EPD_Init() override;
    void EPD_Clear() override;
    void EPD_Display() override;
    void EPD_DisplayPartBaseImage() override;   // no-op: avoids a long white flash at boot
    void EPD_Init_Partial() override;           // fast-mode (OTP) init
    void EPD_DisplayPart() override;            // full refresh, or a fast update when allowed
    void EPD_DrawColorPixel(uint16_t x, uint16_t y, uint8_t color) override;
    void EPD_SetBuffer(const uint8_t *data, int len) override;

    /* Make the next update a factory full refresh (e.g. after many fast ones). */
    void EPD_ForceFullRefresh() { have_prev = false; }
};
#endif
