#ifndef _I2C_MASTER_H_
#define _I2C_MASTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

int i2c_master_init();
int i2c_master_release();
int i2c_master_send(void* data, size_t len, size_t xTicksToWait);
int i2c_master_recv(void* data, size_t len, size_t xTicksToWait);
int i2c_master_scan();
int i2c_master_dev_available();
bool i2c_master_recover_bus(void);
bool i2c_master_reset_driver(void);
bool i2c_master_lock(uint32_t timeout_ms);
void i2c_master_unlock(void);

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // _I2C_MASTER_H_
