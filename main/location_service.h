// location_service.h
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Service initialization
void location_service_init(void);

// Get location
bool location_service_get(double* lat, double* lon, char* city, size_t city_len);

// Get timezone
bool location_service_get_timezone(char* timezone, size_t timezone_len);

// Detect timezone by city
const char* location_service_detect_timezone_by_city(const char* city);

// Get location by IP
bool location_service_get_by_ip(void);

// Set location manually
void location_service_set_manual(double lat, double lon, const char* city);

// Convert IANA to POSIX format
const char* location_service_convert_iana_to_posix(const char* iana_tz);

// Check Wi-Fi connection (uses system Wi-Fi)
bool location_service_is_wifi_connected(void);
void location_service_wait_for_wifi(void);

// Time (uses system time)
uint32_t location_service_get_time(void);
bool location_service_is_time_synced(void);

#ifdef __cplusplus
}
#endif