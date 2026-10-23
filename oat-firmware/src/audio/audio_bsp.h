#ifndef AUDIO_BSP_H
#define AUDIO_BSP_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initialize the ES8311 codec for microphone recording only.
// Call this after i2c_sensor_init() if the I2C bus is already up,
// or standalone if only sound is enabled.
// Returns true on success.
bool audio_bsp_init(void);

// Close the codec device (gates I2S clocks to save power during sleep).
void audio_bsp_close(void);

// Re-open the codec device after close (restores I2S clocks + gain).
bool audio_bsp_reopen(void);

// Read a block of PCM samples from the microphone (16-bit stereo, 16 kHz).
// buf      : caller-allocated buffer
// buf_len  : byte length of buf
// Returns true if bytes were received.
bool audio_bsp_read(void *buf, uint32_t buf_len);

#ifdef __cplusplus
}
#endif

#endif // AUDIO_BSP_H
