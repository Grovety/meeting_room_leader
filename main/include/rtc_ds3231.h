// rtc_ds3231.h
#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <time.h>

/* Uses the same I2C port as in main.cpp: I2C_NUM_0 */
#define RTC_DS3231_ADDR 0x68

/* Initialization (optional, but provides a unified interface) */
esp_err_t rtc_ds3231_init(void);

/* Read epoch from RTC. Returns true on success and if the time is valid (> 2000-01-01). */
bool rtc_ds3231_read_epoch(time_t* out_epoch);

/* Write epoch to RTC (local time). Returns true on success. */
bool rtc_ds3231_write_epoch(time_t epoch);