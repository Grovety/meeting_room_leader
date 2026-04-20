#ifndef DEVICE_NAME_STORE_H
#define DEVICE_NAME_STORE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_NAME_STORE_MAX_LEN 32

const char *device_name_store_get(void);
esp_err_t device_name_store_set(const char *name);

#ifdef __cplusplus
}
#endif

#endif
