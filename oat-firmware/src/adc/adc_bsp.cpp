#include "hal/adc_types.h"
#include <stdio.h>
#include "adc_bsp.h"
#include "user_config.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define ADC_Calibrate

#ifdef ADC_Calibrate
  static adc_cali_handle_t cali_handle;
  static adc_cali_handle_t cali2_handle;
#endif
  static adc_oneshot_unit_handle_t adc1_handle;
  static adc_oneshot_unit_handle_t adc2_handle;
  static bool adc_inited  = false;
  static bool adc2_inited = false;

void adc_bsp_init(void)
{
  if (adc_inited) return;  // idempotent
#ifdef ADC_Calibrate
  adc_cali_curve_fitting_config_t cali_config =
  {
    .unit_id = ADC_UNIT_1,
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali_config, &cali_handle));
#endif
  adc_oneshot_unit_init_cfg_t init_config1 = {
    .unit_id = ADC_UNIT_1,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));
  adc_oneshot_chan_cfg_t config = {
    .atten = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, VBAT_ADC_CHANNEL, &config));
  adc_inited = true;
}

void adc_channel_add(adc_channel_t channel)
{
  adc_oneshot_chan_cfg_t config = {
    .atten    = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, channel, &config));
}

int adc_channel_read_mv(adc_channel_t channel, int *raw_out)
{
  int raw = 0;
  if (adc_oneshot_read(adc1_handle, channel, &raw) != ESP_OK) {
    if (raw_out) *raw_out = 0;
    return 0;
  }
  if (raw_out) *raw_out = raw;
#ifdef ADC_Calibrate
  int mv = 0;
  adc_cali_raw_to_voltage(cali_handle, raw, &mv);
  return mv;
#else
  return (int)((float)raw * 3300.0f / 4096.0f);
#endif
}
void adc2_bsp_init(void)
{
  if (adc2_inited) return;  // idempotent
#ifdef ADC_Calibrate
  adc_cali_curve_fitting_config_t cali2_config = {
    .unit_id  = ADC_UNIT_2,
    .atten    = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cali2_config, &cali2_handle));
#endif
  adc_oneshot_unit_init_cfg_t init_config2 = {
    .unit_id = ADC_UNIT_2,
  };
  ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config2, &adc2_handle));
  adc2_inited = true;
}

void adc2_channel_add(adc_channel_t channel)
{
  adc_oneshot_chan_cfg_t config = {
    .atten    = ADC_ATTEN_DB_12,
    .bitwidth = ADC_BITWIDTH_12,
  };
  ESP_ERROR_CHECK(adc_oneshot_config_channel(adc2_handle, channel, &config));
}

int adc2_channel_read_mv(adc_channel_t channel, int *raw_out)
{
  int raw = 0;
  if (adc_oneshot_read(adc2_handle, channel, &raw) != ESP_OK) {
    if (raw_out) *raw_out = 0;
    return 0;
  }
  if (raw_out) *raw_out = raw;
#ifdef ADC_Calibrate
  int mv = 0;
  adc_cali_raw_to_voltage(cali2_handle, raw, &mv);
  return mv;
#else
  return (int)((float)raw * 3300.0f / 4096.0f);
#endif
}

void adc_get_value(float *value,int *data)
{
  int adcdata;
#ifdef ADC_Calibrate
  int vol = 0;
#endif
  esp_err_t err;
  err = adc_oneshot_read(adc1_handle,VBAT_ADC_CHANNEL,&adcdata);
  if(err == ESP_OK)
  {
#ifdef ADC_Calibrate
    adc_cali_raw_to_voltage(cali_handle,adcdata,&vol);
    *value = 0.001 * vol * VBAT_DIVIDER_RATIO;
#else
    *value = ((float)adcdata * 3.3/4096) * VBAT_DIVIDER_RATIO;
#endif
    if(data != NULL)
    {*data = adcdata;}
  }
  else
  {
    *value = 0;
    if(data != NULL)
    {*data = 0;}
  }
}

int battery_read_mv(void)
{
  adc_bsp_init();  // idempotent; also configures VBAT_ADC_CHANNEL

  // The 200K/200K divider is a high-impedance source for the SAR ADC, so a
  // single conversion is noisy. Average a short burst instead.
  const int N = 16;
  long sum = 0;
  int  n   = 0;
  for (int i = 0; i < N; i++) {
    int mv = adc_channel_read_mv(VBAT_ADC_CHANNEL, NULL);
    if (mv > 0) { sum += mv; n++; }
  }
  if (n == 0) return 0;
  return (int)((float)(sum / n) * VBAT_DIVIDER_RATIO * VBAT_SCALE + 0.5f);
}

int battery_percent(int vbat_mv)
{
  // Typical single-cell LiPo open-circuit voltage vs. state of charge.
  // Linear interpolation between points. Accuracy is roughly +/-10%, worst in
  // the flat 3.7-3.9 V region, and reads high while the charger is active.
  static const struct { int mv; int pct; } tbl[] = {
    {4200, 100}, {4100, 90}, {4000, 78}, {3900, 62}, {3800, 45},
    {3750,  35}, {3700, 25}, {3650, 15}, {3600,  8}, {3500,  3},
    {3300,   0},
  };
  const int N = sizeof(tbl) / sizeof(tbl[0]);
  if (vbat_mv >= tbl[0].mv) return 100;
  for (int i = 1; i < N; i++) {
    if (vbat_mv >= tbl[i].mv) {
      int dv = tbl[i-1].mv  - tbl[i].mv;
      int dp = tbl[i-1].pct - tbl[i].pct;
      return tbl[i].pct + (vbat_mv - tbl[i].mv) * dp / dv;
    }
  }
  return 0;
}
