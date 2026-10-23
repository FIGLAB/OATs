#include "audio_bsp.h"
#include "../codec_board/codec_board.h"
#include "../codec_board/codec_init.h"
#include "../esp_codec_dev/include/esp_codec_dev.h"
#include "esp_log.h"

static const char *TAG = "audio_bsp";

static esp_codec_dev_handle_t record_dev = NULL;
static bool is_open = false;

bool audio_bsp_init(void)
{
    set_codec_board_type("S3_ePaper_1_54");

    codec_init_cfg_t cfg = {};
    cfg.in_mode   = CODEC_I2S_MODE_STD; // microphone input
    cfg.out_mode  = CODEC_I2S_MODE_STD; // ES8311 is in_out so playback side is needed too
    cfg.in_use_tdm = false;
    cfg.reuse_dev  = false;

    int ret = init_codec(&cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "init_codec failed (%d)", ret);
        return false;
    }

    record_dev = get_record_handle();
    if (record_dev == NULL) {
        ESP_LOGE(TAG, "No record handle");
        return false;
    }

    esp_codec_dev_set_in_gain(record_dev, 30.0f);

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate    = 16000;
    fs.channel        = 2;
    fs.bits_per_sample = 16;

    int err = esp_codec_dev_open(record_dev, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed (%d)", err);
        return false;
    }

    is_open = true;
    ESP_LOGI(TAG, "Microphone ready (16kHz, 16-bit, stereo)");
    return true;
}

void audio_bsp_close(void)
{
    if (is_open && record_dev != NULL) {
        esp_codec_dev_close(record_dev);
        is_open = false;
    }
}

bool audio_bsp_reopen(void)
{
    if (record_dev == NULL) return false;
    if (is_open) return true;

    esp_codec_dev_set_in_gain(record_dev, 30.0f);

    esp_codec_dev_sample_info_t fs = {};
    fs.sample_rate     = 16000;
    fs.channel         = 2;
    fs.bits_per_sample = 16;

    int err = esp_codec_dev_open(record_dev, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open failed (%d)", err);
        return false;
    }
    is_open = true;
    return true;
}

bool audio_bsp_read(void *buf, uint32_t buf_len)
{
    if (!is_open || record_dev == NULL) return false;
    return (esp_codec_dev_read(record_dev, buf, buf_len) == ESP_CODEC_DEV_OK);
}
