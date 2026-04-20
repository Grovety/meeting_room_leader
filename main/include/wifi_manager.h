#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// THIS SECTION IS IMPORTANT
#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_manager_init_sta(const char* ssid, const char* pass);
bool wifi_manager_is_connected(void);
void wifi_manager_wait_connected(void);
esp_err_t wifi_manager_disconnect(bool forget_credentials);
esp_err_t wifi_manager_save_credentials(const char* ssid, const char* pass);
esp_err_t wifi_manager_fetch_credentials(char* ssid, char* pass);
bool wifi_manager_lock(uint32_t timeout_ms);
void wifi_manager_unlock(void);
void wifi_manager_set_auto_reconnect_enabled(bool enabled);
bool wifi_manager_is_auto_reconnect_enabled(void);

#ifdef __cplusplus
}
#endif

#endif // WIFI_MANAGER_H
