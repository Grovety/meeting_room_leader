#include "network_request_guard.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_network_request_mutex = NULL;

static SemaphoreHandle_t ensure_network_request_mutex(void)
{
    if (!s_network_request_mutex) {
        s_network_request_mutex = xSemaphoreCreateMutex();
    }
    return s_network_request_mutex;
}

bool network_request_guard_lock(uint32_t timeout_ms)
{
    SemaphoreHandle_t mutex = ensure_network_request_mutex();
    if (!mutex) {
        return false;
    }

    TickType_t wait_ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(mutex, wait_ticks) == pdTRUE;
}

void network_request_guard_unlock(void)
{
    if (s_network_request_mutex) {
        xSemaphoreGive(s_network_request_mutex);
    }
}
