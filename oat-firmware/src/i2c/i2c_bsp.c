#include <stdio.h>
#include "i2c_bsp.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "user_config.h"

static const char *TAG = "i2c_bsp";

static i2c_master_bus_handle_t i2c_bus_handle = NULL;
i2c_master_dev_handle_t shtc3_handle = NULL;

static const uint32_t I2C_TIMEOUT_MS = 5000;
static const uint32_t I2C_DONE_MS   = 1000;

void i2c_sensor_init(void)
{
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.clk_source              = I2C_CLK_SRC_DEFAULT;
    bus_cfg.i2c_port                = ESP32_I2C_DEV_NUM;
    bus_cfg.scl_io_num              = ESP32_I2C_SCL_PIN;
    bus_cfg.sda_io_num              = ESP32_I2C_SDA_PIN;
    bus_cfg.glitch_ignore_cnt       = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &i2c_bus_handle));
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d SCL=%d)", ESP32_I2C_SDA_PIN, ESP32_I2C_SCL_PIN);

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = I2C_SHTC3_DEV_Address;
    dev_cfg.scl_speed_hz    = 300000;

    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, &shtc3_handle));
    ESP_LOGI(TAG, "SHTC3 device registered at 0x%02X", I2C_SHTC3_DEV_Address);
}

i2c_master_bus_handle_t i2c_get_bus_handle(void)
{
    return i2c_bus_handle;
}

int i2c_write_buff(i2c_master_dev_handle_t dev_handle, int reg, uint8_t *buf, uint8_t len)
{
    int ret;
    i2c_master_bus_wait_all_done(i2c_bus_handle, pdMS_TO_TICKS(I2C_DONE_MS));
    if (reg == -1) {
        ret = i2c_master_transmit(dev_handle, buf, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    } else {
        uint8_t *pbuf = (uint8_t *)malloc(len + 1);
        pbuf[0] = (uint8_t)reg;
        for (uint8_t i = 0; i < len; i++) pbuf[i + 1] = buf[i];
        ret = i2c_master_transmit(dev_handle, pbuf, len + 1, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
        free(pbuf);
    }
    return ret;
}

int i2c_read_buff(i2c_master_dev_handle_t dev_handle, int reg, uint8_t *buf, uint8_t len)
{
    int ret;
    i2c_master_bus_wait_all_done(i2c_bus_handle, pdMS_TO_TICKS(I2C_DONE_MS));
    if (reg == -1) {
        ret = i2c_master_receive(dev_handle, buf, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    } else {
        uint8_t addr = (uint8_t)reg;
        ret = i2c_master_transmit_receive(dev_handle, &addr, 1, buf, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    }
    return ret;
}

int i2c_write_read(i2c_master_dev_handle_t dev_handle, uint8_t *write_buf, uint8_t write_len, uint8_t *read_buf, uint8_t read_len)
{
    i2c_master_bus_wait_all_done(i2c_bus_handle, pdMS_TO_TICKS(I2C_DONE_MS));
    return i2c_master_transmit_receive(dev_handle, write_buf, write_len, read_buf, read_len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
}
