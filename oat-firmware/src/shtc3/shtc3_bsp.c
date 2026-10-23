#include "shtc3_bsp.h"
#include "../i2c/i2c_bsp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "shtc3";

// SHTC3 command words (big-endian, sent MSB first)
#define CMD_WAKEUP          0x3517
#define CMD_SOFT_RESET      0x805D
#define CMD_SLEEP           0xB098
#define CMD_READ_ID         0xEFC8
// T first, clock-stretching disabled (polling mode)
#define CMD_MEAS_T_RH       0x7866

#define CRC_POLYNOMIAL      0x131u

static uint8_t calc_crc(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++)
            crc = (crc & 0x80) ? (crc << 1) ^ CRC_POLYNOMIAL : (crc << 1);
    }
    return crc;
}

static void send_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    i2c_write_buff(shtc3_handle, -1, buf, 2);
}

bool shtc3_init(void)
{
    send_cmd(CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(50));
    send_cmd(CMD_SOFT_RESET);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Read ID to verify sensor is present
    uint8_t cmd[2] = { (uint8_t)(CMD_READ_ID >> 8), (uint8_t)(CMD_READ_ID & 0xFF) };
    uint8_t id_buf[3] = {0};
    int err = i2c_write_read(shtc3_handle, cmd, 2, id_buf, 3);
    if (err != 0) {
        ESP_LOGE(TAG, "SHTC3 not found (err %d)", err);
        return false;
    }
    if (calc_crc(id_buf, 2) != id_buf[2]) {
        ESP_LOGE(TAG, "SHTC3 ID CRC mismatch");
        return false;
    }
    uint16_t id = ((uint16_t)id_buf[0] << 8) | id_buf[1];
    ESP_LOGI(TAG, "SHTC3 ID: 0x%04X", id);

    send_cmd(CMD_SLEEP);
    return true;
}

bool shtc3_read(shtc3_data_t *out)
{
    // Wake up (sensor rail already on for >=50ms, so minimal wakeup delay needed)
    send_cmd(CMD_WAKEUP);
    vTaskDelay(pdMS_TO_TICKS(1));

    // Trigger measurement (T first, polling; datasheet max ~0.8ms normal mode)
    send_cmd(CMD_MEAS_T_RH);
    vTaskDelay(pdMS_TO_TICKS(20));

    // Read 6 bytes: T_MSB T_LSB T_CRC H_MSB H_LSB H_CRC
    uint8_t data[6] = {0};
    int err = i2c_read_buff(shtc3_handle, -1, data, 6);
    if (err != 0) {
        ESP_LOGW(TAG, "SHTC3 read failed (%d)", err);
        send_cmd(CMD_SLEEP);
        return false;
    }

    if (calc_crc(data, 2) != data[2] || calc_crc(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "SHTC3 CRC mismatch");
        send_cmd(CMD_SLEEP);
        return false;
    }

    uint16_t raw_t = ((uint16_t)data[0] << 8) | data[1];
    uint16_t raw_h = ((uint16_t)data[3] << 8) | data[4];

    // From SHTC3 datasheet
    out->temp = 175.0f * raw_t / 65536.0f - 45.0f;
    out->rh   = 100.0f * raw_h / 65536.0f;

    send_cmd(CMD_SLEEP);
    return true;
}
