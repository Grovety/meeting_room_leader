#ifndef AUX_UDP_LINK_H
#define AUX_UDP_LINK_H

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t aux_udp_link_start(void);
esp_err_t aux_udp_link_enable_pairing_window(uint32_t duration_sec);
esp_err_t aux_udp_link_reset_pairing(void);

#ifdef __cplusplus
}
#endif

#endif
