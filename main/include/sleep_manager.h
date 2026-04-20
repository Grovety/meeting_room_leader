#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "port/esp_io_expander.h"

#ifdef __cplusplus
#include "esp_display_panel.hpp"
void sleep_manager_init(esp_panel::board::Board* board);
extern "C" {
#endif

void sleep_manager_set_manual_expander(esp_io_expander_handle_t expander);
void sleep_manager_start(void);

void sleep_manager_enter_sleep(void);
void sleep_manager_exit_sleep(void);
void sleep_manager_reset_inactivity_timer(void);
bool sleep_manager_is_sleeping(void);
void sleep_manager_set_timeout_ms(uint32_t timeout_ms);
uint32_t sleep_manager_get_timeout_ms(void);

#ifdef __cplusplus
}
#endif
