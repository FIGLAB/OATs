#ifndef I2C_BSP_H
#define I2C_BSP_H

#include "driver/i2c_master.h"

extern i2c_master_dev_handle_t shtc3_handle;

#ifdef __cplusplus
extern "C" {
#endif

// Initialize I2C bus (I2C_NUM_0, SDA=47, SCL=48) and register the SHTC3 device.
void i2c_sensor_init(void);

// Returns the raw bus handle so other drivers (e.g. codec) can add their own devices.
i2c_master_bus_handle_t i2c_get_bus_handle(void);

int i2c_write_buff(i2c_master_dev_handle_t dev_handle, int reg, uint8_t *buf, uint8_t len);
int i2c_read_buff(i2c_master_dev_handle_t dev_handle, int reg, uint8_t *buf, uint8_t len);
int i2c_write_read(i2c_master_dev_handle_t dev_handle, uint8_t *write_buf, uint8_t write_len, uint8_t *read_buf, uint8_t read_len);

#ifdef __cplusplus
}
#endif

#endif // I2C_BSP_H
