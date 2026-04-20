#ifndef CONFIG_PORTAL_H
#define CONFIG_PORTAL_H

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CONFIG_PORTAL_PHASE_IDLE = 0,
    CONFIG_PORTAL_PHASE_STARTING,
    CONFIG_PORTAL_PHASE_RUNNING,
    CONFIG_PORTAL_PHASE_APPLYING,
    CONFIG_PORTAL_PHASE_STOPPING,
    CONFIG_PORTAL_PHASE_ERROR,
} config_portal_phase_t;

typedef enum {
    CONFIG_PORTAL_ACCESS_NONE = 0,
    CONFIG_PORTAL_ACCESS_AP,
    CONFIG_PORTAL_ACCESS_LOCAL,
} config_portal_access_mode_t;

esp_err_t config_portal_request_boot_once(void);
bool config_portal_consume_boot_request(void);
esp_err_t config_portal_reboot_into_setup_mode(void);

esp_err_t config_portal_start(void);
esp_err_t config_portal_start_runtime(void);
void config_portal_stop(void);
esp_err_t config_portal_stop_runtime(void);
bool config_portal_is_running(void);
bool config_portal_is_runtime_mode(void);
config_portal_access_mode_t config_portal_get_access_mode(void);
bool config_portal_requires_setup_wifi(void);
config_portal_phase_t config_portal_get_phase(void);
void config_portal_get_status_copy(char* out, size_t out_size);

const char* config_portal_get_ap_ssid(void);
const char* config_portal_get_ap_password(void);
const char* config_portal_get_ap_url(void);
const char* config_portal_get_network_name(void);

void config_portal_show_screen(bool portal_ready, const char* detail);

#ifdef __cplusplus
}
#endif

#endif
