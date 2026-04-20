#pragma once
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

bool time_sync_start(const char* iana_tz, const char* city);
bool time_sync_is_synced(void);
void time_sync_apply_saved_timezone(void);

#ifdef __cplusplus
}
#endif
