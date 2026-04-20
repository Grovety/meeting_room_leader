#include "lv_mem_psram.h"

/* LVGL's allocator is used for objects/styles/strings and does not require DMA-capable memory.
 * Prefer PSRAM to preserve internal RAM for display draw buffers and system tasks. */
#ifndef LV_HEAP_CAPS_PRIMARY
#define LV_HEAP_CAPS_PRIMARY  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#ifndef LV_HEAP_CAPS_FALLBACK
#define LV_HEAP_CAPS_FALLBACK (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif

void * lv_mem_custom_alloc(size_t size) {
    return heap_caps_malloc_prefer(size, 2, LV_HEAP_CAPS_PRIMARY, LV_HEAP_CAPS_FALLBACK);
}

void * lv_mem_custom_realloc(void * p, size_t new_size) {
    return heap_caps_realloc_prefer(p, new_size, 2, LV_HEAP_CAPS_PRIMARY, LV_HEAP_CAPS_FALLBACK);
}

void lv_mem_custom_free(void * p) {
    heap_caps_free(p);
}
