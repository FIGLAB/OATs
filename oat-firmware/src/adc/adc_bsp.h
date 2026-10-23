#ifndef ADC_BSP_H
#define ADC_BSP_H

#include <Arduino.h>
#include "hal/adc_types.h"

// Initialize ADC1 unit + calibration (idempotent — safe to call more than once).
void adc_bsp_init(void);

// Read battery voltage via VBAT_ADC_CHANNEL (GPIO4). value = volts at the
// battery (divider compensated), data = 12-bit raw. Single sample, unfiltered.
void adc_get_value(float *value, int *data);

// Battery voltage in millivolts (divider compensated). Averages a burst of
// samples. Initialises ADC1 on first use, so it is safe any time after boot.
// Returns 0 on error.
int battery_read_mv(void);

// Estimate state of charge (0-100) from a resting LiPo voltage in millivolts.
int battery_percent(int vbat_mv);

// Configure an additional ADC1 channel (call after adc_bsp_init).
void adc_channel_add(adc_channel_t channel);

// Read millivolts from any configured ADC1 channel.
// raw_out may be NULL. Returns 0 on error.
int adc_channel_read_mv(adc_channel_t channel, int *raw_out);

// Initialize ADC2 unit + calibration (idempotent). Call only after WiFi is off.
void adc2_bsp_init(void);

// Configure an ADC2 channel (call after adc2_bsp_init).
void adc2_channel_add(adc_channel_t channel);

// Read millivolts from any configured ADC2 channel.
// raw_out may be NULL. Returns 0 on error.
int adc2_channel_read_mv(adc_channel_t channel, int *raw_out);

#endif
