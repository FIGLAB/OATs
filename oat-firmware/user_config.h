#ifndef USER_CONFIG_H
#define USER_CONFIG_H

#define EPD_SPI_NUM        SPI2_HOST
#define ESP32_I2C_DEV_NUM  I2C_NUM_0

#define EPD_WIDTH  200
#define EPD_HEIGHT 200
/*Which 1.54" panel: 0 = auto-detect at boot (BUSY idle level after reset),
  1 = force black/white (ESP32-S3-ePaper-1.54), 2 = force 4-color (…-1.54G)*/
#define EPD_PANEL  0
/*Experimental fast black/white updates on the 4-color (G) panel.
  Instead of the factory 15 s full refresh, the driver uploads a frame of
  transition codes (stays / to black / to white) and runs a short waveform of
  its own through the controller's "LUT from register" mode, like the partial
  refresh of the black/white board. The register layout is not in the public
  JD79660A datasheet: run tools/g_lut_probe on the device first and copy the
  values that worked here. A factory full refresh still runs at boot and
  periodically (see the last two knobs) to clear ghosting.
  0 = off (factory full refresh only, safe default), 1 = on.*/
#define EPD_G_FAST_UPDATE        0
/*LUT register byte layout: 1 = 7-byte groups (JD79651/JD79653 style),
  2 = 6-byte groups (JD79686 style). Probe command: f*/
#define EPD_G_LUT_FORMAT         1
/*Format 1 only: byte 0 of a used group (0 = run once). Probe command: g*/
#define EPD_G_LUT_GROUP_REPEAT   0
/*Which LUT register drives which pixel code 0..3. Probe command: m*/
#define EPD_G_CODE_REGS          {0x21, 0x22, 0x23, 0x24}
/*1 = black with VSP (+15 V), white with a weak negative field (E Ink patent
  polarity); 2 = black with VSN, white with a weak positive. Probe command: b*/
#define EPD_G_POLARITY           1
/*Polarity 1 only. How the weak negative field is made: 1 = VCOM at VSPL (3 V)
  and source at 0 V; 2 = VCOM at VSP and source at VSPL (12 V). Probe: s*/
#define EPD_G_WHITE_SCHEME       1
/*Frame clock (PLL register): 0x02 = 50 Hz, 20 ms per frame.*/
#define EPD_G_PLL                0x02
/*Frame clock for the factory full refresh (used whether or not the fast path
  is on). Waveshare's 0x08 lets the controller vary the rate per phase: 12.4 s
  per refresh. A fixed rate runs the same waveform faster with the same
  picture: 0x07 = 120 Hz, 8.6 s (measured); 0x06 = 100 Hz, 10.4 s;
  0x05 = 85 Hz, 12.3 s. Values 0x00-0x04 are slower than the factory setting.*/
#define EPD_G_FULL_PLL           0x07
/*Pulse lengths in frames. 15 V x black = 3 V x white keeps the DC balance.*/
#define EPD_G_BLACK_FRAMES       10
#define EPD_G_WHITE_FRAMES       50
/*Factory full refresh after this many fast updates, or minutes since the
  last one, whichever comes first.*/
#define EPD_G_FAST_MAX_UPDATES   20
#define EPD_G_FULL_REFRESH_MIN   60
#define LVGL_SPIRAM_BUFF_LEN (EPD_WIDTH * EPD_HEIGHT * 2)

/*EPD port Init*/
#define EPD_DC_PIN    GPIO_NUM_10
#define EPD_CS_PIN    GPIO_NUM_11
#define EPD_SCK_PIN   GPIO_NUM_12
#define EPD_MOSI_PIN  GPIO_NUM_13
#define EPD_RST_PIN   GPIO_NUM_9
#define EPD_BUSY_PIN  GPIO_NUM_8


/*DEV POWER init*/
#define EPD_PWR_PIN     GPIO_NUM_6
#define Audio_PWR_PIN   GPIO_NUM_42
#define VBAT_PWR_PIN    GPIO_NUM_17

/*Battery sense (on the Waveshare module, not on the expansion header):
  GPIO4 = BAT_ADC through a 200K/200K divider, so VBAT = 2 x VADC.
  GPIO4 is ADC1_CH3 on the ESP32-S3.*/
#define VBAT_ADC_CHANNEL    ADC_CHANNEL_3
#define VBAT_DIVIDER_RATIO  2
/*One-point calibration: multimeter volts / firmware volts. The 1% divider
  resistors alone can put the reading off by 2%, so a full 4.20 V pack may
  read 4.07 V; in that case set this to 4.20/4.07 = 1.032.*/
#define VBAT_SCALE          1.000f

#define BOOT_BUTTON_PIN GPIO_NUM_0
#define PWR_BUTTON_PIN  GPIO_NUM_18

/*Low-power wake-up*/
#define ext_wakeup_pin_1 GPIO_NUM_0

/*ESP32 I2C Init*/
#define ESP32_I2C_SDA_PIN GPIO_NUM_47
#define ESP32_I2C_SCL_PIN GPIO_NUM_48

/*lvgl init*/
#define EXAMPLE_LVGL_TICK_PERIOD_MS    5
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS 500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS 100

/*i2c dev*/
#define I2C_RTC_DEV_Address        0x51
#define I2C_SHTC3_DEV_Address      0x70

/*GPIO sensor pins (pin1–pin4 in sensors.json)*/
#define SENSOR_PIN1   GPIO_NUM_1
#define SENSOR_PIN2   GPIO_NUM_2
#define SENSOR_PIN3   GPIO_NUM_3
#define SENSOR_PIN4   GPIO_NUM_19

/*Sensor power rail — HIGH = 3.3 V on, LOW = off*/
#define SENSOR_PWR_PIN  GPIO_NUM_20

#endif // !USER_CONFIG_H