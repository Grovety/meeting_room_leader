#ifndef NETWORK_REQUEST_GUARD_H
#define NETWORK_REQUEST_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool network_request_guard_lock(uint32_t timeout_ms);
void network_request_guard_unlock(void);

#ifdef __cplusplus
}
#endif

#endif
