#ifndef SHTC3_BSP_H
#define SHTC3_BSP_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    float temp; // °C
    float rh;   // % relative humidity
} shtc3_data_t;

#ifdef __cplusplus
extern "C" {
#endif

// Initialize and verify the SHTC3 (call after i2c_sensor_init).
// Returns true if the sensor responded correctly.
bool shtc3_init(void);

// Wake, measure, sleep. Returns true on success.
bool shtc3_read(shtc3_data_t *out);

#ifdef __cplusplus
}
#endif

#endif // SHTC3_BSP_H
