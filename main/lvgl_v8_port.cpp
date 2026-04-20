/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * Modified: disable SW-rotation, increase buffers aggressively, set max LVGL task priority,
 * non-blocking touch polling, profiling logs.
 */
#include "esp_timer.h"
#undef ESP_UTILS_LOG_TAG
#define ESP_UTILS_LOG_TAG "LvPort"
#include "esp_lib_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "i2c_master.h"
#include "lvgl_v8_port.h"
#include "esp_heap_caps.h"
#include <inttypes.h>
#include <string.h>
using namespace esp_panel::drivers;

#define LVGL_PORT_ENABLE_ROTATION_OPTIMIZED (1)
#define LVGL_PORT_BUFFER_NUM_MAX            (2)

// runtime flags
static bool g_disable_sw_rotation __attribute__((unused)) = true;    // SW-rotation disabled at runtime
static SemaphoreHandle_t lvgl_mux                         = nullptr; // LVGL mutex
static TaskHandle_t lvgl_task_handle                      = nullptr;
static TaskHandle_t touch_poll_task_handle                = nullptr;
static esp_timer_handle_t lvgl_tick_timer                 = NULL;
static void* lvgl_buf[LVGL_PORT_BUFFER_NUM_MAX]           = {};
static LCD* global_lcd_ptr                                = nullptr;

// cached touch (polling)
static volatile int s_touch_x        = 0;
static volatile int s_touch_y        = 0;
static volatile bool s_touch_pressed = false;

static_assert(LVGL_PORT_TASK_PRIORITY <= (configMAX_PRIORITIES - 1),
              "LVGL task priority must not exceed FreeRTOS maximum");

#if LVGL_PORT_ROTATION_DEGREE != 0
static void* get_next_frame_buffer(LCD* lcd)
{
    static void* next_fb = NULL;
    static void* fbs[2]  = {NULL};
    if (next_fb == NULL) {
        fbs[0]  = lcd->getFrameBufferByIndex(0);
        fbs[1]  = lcd->getFrameBufferByIndex(1);
        next_fb = fbs[1];
    } else {
        next_fb = (next_fb == fbs[0]) ? fbs[1] : fbs[0];
    }
    return next_fb;
}

__attribute__((always_inline)) static inline void copy_pixel_8bpp(uint8_t* to, const uint8_t* from) { *to++ = *from++; }
__attribute__((always_inline)) static inline void copy_pixel_16bpp(uint8_t* to, const uint8_t* from)
{
    *(uint16_t*)to++ = *(const uint16_t*)from++;
}
__attribute__((always_inline)) static inline void copy_pixel_24bpp(uint8_t* to, const uint8_t* from)
{
    *to++ = *from++;
    *to++ = *from++;
    *to++ = *from++;
}

#define _COPY_PIXEL(_bpp, to, from) copy_pixel_##_bpp##bpp(to, from)
#define COPY_PIXEL(_bpp, to, from)  _COPY_PIXEL(_bpp, to, from)

#define ROTATE_90_ALL_BPP()                                                                                            \
    {                                                                                                                  \
        to_bytes_per_line = h * to_bytes_per_piexl;                                                                    \
        to_index_const    = (w - x_start - 1) * to_bytes_per_line;                                                     \
        for (int from_y = y_start; from_y < y_end + 1; from_y++) {                                                     \
            from_index = from_y * from_bytes_per_line + x_start * from_bytes_per_piexl;                                \
            to_index   = to_index_const + from_y * to_bytes_per_piexl;                                                 \
            for (int from_x_local = x_start; from_x_local <= x_end; from_x_local++) {                                  \
                COPY_PIXEL(LV_COLOR_DEPTH, to + to_index, from + from_index);                                          \
                from_index += from_bytes_per_piexl;                                                                    \
                to_index -= to_bytes_per_line;                                                                         \
            }                                                                                                          \
        }                                                                                                              \
    }

/**
 * @brief Optimized transpose function for RGB565 format.
 *
 */
#define ROTATE_90_OPTIMIZED_16BPP(block_w, block_h)                                                                    \
    {                                                                                                                  \
        for (int i = 0; i < h; i += block_h) {                                                                         \
            max_height = (i + block_h > h) ? h : (i + block_h);                                                        \
            for (int j = 0; j < w; j += block_w) {                                                                     \
                max_width = (j + block_w > w) ? w : (j + block_w);                                                     \
                start_y   = w - 1 - j;                                                                                 \
                for (int x = i; x < max_height; x++) {                                                                 \
                    from_next = (uint16_t*)from + x * w;                                                               \
                    for (int y = j, mirrored_y = start_y; y < max_width; y += 4, mirrored_y -= 4) {                    \
                        ((uint16_t*)to)[(mirrored_y) * h + x]     = *((uint32_t*)(from_next + y)) & 0xFFFF;            \
                        ((uint16_t*)to)[(mirrored_y - 1) * h + x] = (*((uint32_t*)(from_next + y)) >> 16) & 0xFFFF;    \
                        ((uint16_t*)to)[(mirrored_y - 2) * h + x] = *((uint16_t*)(from_next + y + 2)) & 0xFFFF;        \
                        ((uint16_t*)to)[(mirrored_y - 3) * h + x] =                                                    \
                            (*((uint32_t*)(from_next + y + 2)) >> 16) & 0xFFFF;                                        \
                    }                                                                                                  \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    }

#define ROTATE_180_ALL_BPP()                                                                                           \
    {                                                                                                                  \
        to_bytes_per_line = w * to_bytes_per_piexl;                                                                    \
        to_index_const    = (h - 1) * to_bytes_per_line + (w - x_start - 1) * to_bytes_per_piexl;                      \
        for (int from_y = y_start; from_y < y_end + 1; from_y++) {                                                     \
            from_index = from_y * from_bytes_per_line + x_start * from_bytes_per_piexl;                                \
            to_index   = to_index_const - from_y * to_bytes_per_line;                                                  \
            for (int from_x_local = x_start; from_x_local <= x_end; from_x_local++) {                                  \
                COPY_PIXEL(LV_COLOR_DEPTH, to + to_index, from + from_index);                                          \
                from_index += from_bytes_per_piexl;                                                                    \
                to_index -= to_bytes_per_piexl;                                                                        \
            }                                                                                                          \
        }                                                                                                              \
    }

#define ROTATE_270_OPTIMIZED_16BPP(block_w, block_h)                                                                   \
    {                                                                                                                  \
        for (int i = 0; i < h; i += block_h) {                                                                         \
            max_height = i + block_h > h ? h : i + block_h;                                                            \
            for (int j = 0; j < w; j += block_w) {                                                                     \
                max_width = j + block_w > w ? w : j + block_w;                                                         \
                for (int x = i; x < max_height; x++) {                                                                 \
                    from_next = (uint16_t*)from + x * w;                                                               \
                    for (int y = j; y < max_width; y += 4) {                                                           \
                        ((uint16_t*)to)[y * h + (h - 1 - x)]       = *((uint32_t*)(from_next + y)) & 0xFFFF;           \
                        ((uint16_t*)to)[(y + 1) * h + (h - 1 - x)] = (*((uint32_t*)(from_next + y)) >> 16) & 0xFFFF;   \
                        ((uint16_t*)to)[(y + 2) * h + (h - 1 - x)] = *((uint16_t*)(from_next + y + 2)) & 0xFFFF;       \
                        ((uint16_t*)to)[(y + 3) * h + (h - 1 - x)] =                                                   \
                            (*((uint32_t*)(from_next + y + 2)) >> 16) & 0xFFFF;                                        \
                    }                                                                                                  \
                }                                                                                                      \
            }                                                                                                          \
        }                                                                                                              \
    }

#define ROTATE_270_ALL_BPP()                                                                                           \
    {                                                                                                                  \
        to_bytes_per_line = h * to_bytes_per_piexl;                                                                    \
        from_index_const  = x_start * from_bytes_per_piexl;                                                            \
        to_index_const    = x_start * to_bytes_per_line + (h - 1) * to_bytes_per_piexl;                                \
        for (int from_y = y_start; from_y < y_end + 1; from_y++) {                                                     \
            from_index = from_y * from_bytes_per_line + from_index_const;                                              \
            to_index   = to_index_const - from_y * to_bytes_per_piexl;                                                 \
            for (int from_x_local = x_start; from_x_local <= x_end; from_x_local++) {                                  \
                COPY_PIXEL(LV_COLOR_DEPTH, to + to_index, from + from_index);                                          \
                from_index += from_bytes_per_piexl;                                                                    \
                to_index += to_bytes_per_line;                                                                         \
            }                                                                                                          \
        }                                                                                                              \
    }

__attribute__((always_inline)) IRAM_ATTR static inline void rotate_copy_pixel(const uint8_t* from, uint8_t* to,
                                                                              uint16_t x_start, uint16_t y_start,
                                                                              uint16_t x_end, uint16_t y_end,
                                                                              uint16_t w, uint16_t h, uint16_t rotate)
{
    int from_bytes_per_piexl = sizeof(lv_color_t);
    int from_bytes_per_line  = w * from_bytes_per_piexl;
    int from_index           = 0;
    int to_bytes_per_piexl   = LV_COLOR_DEPTH >> 3;
    int to_bytes_per_line;
    int to_index       = 0;
    int to_index_const = 0;
#if (LV_COLOR_DEPTH == 16) && LVGL_PORT_ENABLE_ROTATION_OPTIMIZED
    int max_height      = 0;
    int max_width       = 0;
    int start_y         = 0;
    uint16_t* from_next = NULL;
#endif
    switch (rotate) {
    case 90:
#if (LV_COLOR_DEPTH == 16) && LVGL_PORT_ENABLE_ROTATION_OPTIMIZED
        ROTATE_90_OPTIMIZED_16BPP(32, 256);
#else
        ROTATE_90_ALL_BPP();
#endif
        break;
    case 180:
        ROTATE_180_ALL_BPP();
        break;
    case 270:
#if (LV_COLOR_DEPTH == 16) && LVGL_PORT_ENABLE_ROTATION_OPTIMIZED
        ROTATE_270_OPTIMIZED_16BPP(32, 256);
#else
        ROTATE_270_ALL_BPP();
#endif
        break;
    default:
        break;
    }
}
#endif /* LVGL_PORT_ROTATION_DEGREE */

/* ---------- touch / input: touchpad_read + indev_init (moved up so lvgl_port_init sees it) ---------- */
static void touchpad_read(lv_indev_drv_t* indev_drv, lv_indev_data_t* data)
{
    // Return cached values populated by touch_poll_task()
    if (s_touch_pressed) {
        data->point.x = s_touch_x;
        data->point.y = s_touch_y;
        data->state   = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

static lv_indev_t* indev_init(Touch* tp)
{
    ESP_UTILS_CHECK_FALSE_RETURN(tp != nullptr, nullptr, "Invalid touch device");
    ESP_UTILS_CHECK_FALSE_RETURN(tp->getPanelHandle() != nullptr, nullptr, "Touch device is not initialized");
    static lv_indev_drv_t indev_drv_tp;
    ESP_UTILS_LOGD("Register input driver to LVGL");
    lv_indev_drv_init(&indev_drv_tp);
    indev_drv_tp.type      = LV_INDEV_TYPE_POINTER;
    indev_drv_tp.read_cb   = touchpad_read; // use cached read from polling task
    indev_drv_tp.user_data = (void*)tp;
    return lv_indev_drv_register(&indev_drv_tp);
}

/* ---------- touch poll task (runs separately to avoid blocking LVGL) ---------- */
static void touch_poll_task(void* arg)
{
    Touch* tp = (Touch*)arg;
    if (! tp) {
        vTaskDelete(NULL);
        return;
    }
    TouchPoint p;
    while (1) {
        int rc = 0;

        // Serialize touch reads with other explicit I2C users to avoid periodic bus contention.
        if (i2c_master_lock(10)) {
            rc = tp->readPoints(&p, 1, 10);
            i2c_master_unlock();
        }
        if (rc > 0) {
            s_touch_x       = p.x;
            s_touch_y       = p.y;
            s_touch_pressed = true;
        } else {
            s_touch_pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

#if LVGL_PORT_AVOID_TEAR
#if LVGL_PORT_DIRECT_MODE
#if LVGL_PORT_ROTATION_DEGREE != 0
typedef struct {
    uint16_t inv_p;
    uint8_t inv_area_joined[LV_INV_BUF_SIZE];
    lv_area_t inv_areas[LV_INV_BUF_SIZE];
} lv_port_dirty_area_t;
static lv_port_dirty_area_t dirty_area;

static void flush_dirty_save(lv_port_dirty_area_t* dirty_area)
{
    lv_disp_t* disp   = _lv_refr_get_disp_refreshing();
    dirty_area->inv_p = disp->inv_p;
    for (int i = 0; i < disp->inv_p; i++) {
        dirty_area->inv_area_joined[i] = disp->inv_area_joined[i];
        dirty_area->inv_areas[i]       = disp->inv_areas[i];
    }
}

typedef enum {
    FLUSH_STATUS_PART,
    FLUSH_STATUS_FULL
} lv_port_flush_status_t;

typedef enum {
    FLUSH_PROBE_PART_COPY,
    FLUSH_PROBE_SKIP_COPY,
    FLUSH_PROBE_FULL_COPY,
} lv_port_flush_probe_t;

/**
 * @brief Probe dirty area to copy
 *
 */
static lv_port_flush_probe_t flush_copy_probe(lv_disp_drv_t* drv)
{
    static lv_port_flush_status_t prev_status = FLUSH_STATUS_PART;
    lv_port_flush_status_t cur_status;
    lv_port_flush_probe_t probe_result;
    lv_disp_t* disp_refr = _lv_refr_get_disp_refreshing();
    uint32_t flush_ver   = 0;
    uint32_t flush_hor   = 0;
    for (int i = 0; i < disp_refr->inv_p; i++) {
        if (disp_refr->inv_area_joined[i] == 0) {
            flush_ver = (disp_refr->inv_areas[i].y2 + 1 - disp_refr->inv_areas[i].y1);
            flush_hor = (disp_refr->inv_areas[i].x2 + 1 - disp_refr->inv_areas[i].x1);
            break;
        }
    }
    /* Check if the current full screen refreshes */
    cur_status =
        ((flush_ver == drv->ver_res) && (flush_hor == drv->hor_res)) ? (FLUSH_STATUS_FULL) : (FLUSH_STATUS_PART);
    if (prev_status == FLUSH_STATUS_FULL) {
        if ((cur_status == FLUSH_STATUS_PART)) {
            probe_result = FLUSH_PROBE_FULL_COPY;
        } else {
            probe_result = FLUSH_PROBE_SKIP_COPY;
        }
    } else {
        probe_result = FLUSH_PROBE_PART_COPY;
    }
    prev_status = cur_status;
    return probe_result;
}

static inline void* flush_get_next_buf(LCD* lcd) { return get_next_frame_buffer(lcd); }

/**
 * @brief Copy dirty area
 *
 */
static void flush_dirty_copy(void* dst, void* src, lv_port_dirty_area_t* dirty_area)
{
    lv_coord_t x_start, x_end, y_start, y_end;
    for (int i = 0; i < dirty_area->inv_p; i++) {
        /* Refresh the unjoined areas*/
        if (dirty_area->inv_area_joined[i] == 0) {
            x_start = dirty_area->inv_areas[i].x1;
            x_end   = dirty_area->inv_areas[i].x2;
            y_start = dirty_area->inv_areas[i].y1;
            y_end   = dirty_area->inv_areas[i].y2;
            if (! g_disable_sw_rotation) {
                rotate_copy_pixel((uint8_t*)src, (uint8_t*)dst, x_start, y_start, x_end, y_end, LV_HOR_RES, LV_VER_RES,
                                  LVGL_PORT_ROTATION_DEGREE);
            } else {
                // SW rotation disabled: copy by full-frame stride to avoid corrupt rows/stripes.
                const size_t pixel_bytes = sizeof(lv_color_t);
                const size_t frame_stride = (size_t)LV_HOR_RES * pixel_bytes;
                const size_t line_bytes = (size_t)(x_end - x_start + 1) * pixel_bytes;
                for (int yy = y_start; yy <= y_end; yy++) {
                    const uint8_t* s = ((const uint8_t*)src) + ((size_t)yy * frame_stride) + ((size_t)x_start * pixel_bytes);
                    uint8_t* d = ((uint8_t*)dst) + ((size_t)yy * frame_stride) + ((size_t)x_start * pixel_bytes);
                    memcpy(d, s, line_bytes);
                }
            }
        }
    }
}

static void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map)
{
    LCD* lcd                           = (LCD*)drv->user_data;
    const int offsetx1                 = area->x1;
    const int offsetx2                 = area->x2;
    const int offsety1                 = area->y1;
    const int offsety2                 = area->y2;
    void* next_fb                      = NULL;
    lv_port_flush_probe_t probe_result = FLUSH_PROBE_PART_COPY;
    lv_disp_t* disp                    = lv_disp_get_default();
    /* Action after last area refresh */
    if (lv_disp_flush_is_last(drv)) {
        /* Check if the `full_refresh` flag has been triggered */
        if (drv->full_refresh) {
            /* Reset flag */
            drv->full_refresh = 0;
            // Rotate and copy data from the whole screen LVGL's buffer to the
            // next frame buffer
            next_fb           = flush_get_next_buf(lcd);
            if (! g_disable_sw_rotation) {
                rotate_copy_pixel((uint8_t*)color_map, (uint8_t*)next_fb, offsetx1, offsety1, offsetx2, offsety2,
                                  LV_HOR_RES, LV_VER_RES, LVGL_PORT_ROTATION_DEGREE);
            } else {
                // SW-rotation disabled: copy directly (assume orientation matches)
                size_t bytes_per_line = (offsetx2 - offsetx1 + 1) * sizeof(lv_color_t);
                for (int yy = offsety1; yy <= offsety2; yy++) {
                    uint8_t* s = ((uint8_t*)color_map) + (yy - offsety1) * bytes_per_line;
                    uint8_t* d =
                        ((uint8_t*)next_fb) + yy * (LV_HOR_RES * sizeof(lv_color_t)) + offsetx1 * sizeof(lv_color_t);
                    memcpy(d, s, bytes_per_line);
                }
            }
            /* Switch the current LCD frame buffer to `next_fb` */
            lcd->switchFrameBufferTo(next_fb);
            /* Wait for the current frame buffer to complete transmission before reusing LVGL buffers. */
            ulTaskNotifyValueClear(NULL, ULONG_MAX);
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            /* Synchronously update the dirty area for another frame buffer */
            flush_dirty_copy(flush_get_next_buf(lcd), color_map, &dirty_area);
            flush_get_next_buf(lcd);
        } else {
            /* Probe the copy method for the current dirty area */
            probe_result = flush_copy_probe(drv);
            if (probe_result == FLUSH_PROBE_FULL_COPY) {
                /* Save current dirty area for next frame buffer */
                flush_dirty_save(&dirty_area);
                /* Set LVGL full-refresh flag and set flush ready in advance */
                drv->full_refresh           = 1;
                disp->rendering_in_progress = false;
                lv_disp_flush_ready(drv);
                /* Force to refresh whole screen, and will invoke
                 * `flush_callback` recursively */
                lv_refr_now(_lv_refr_get_disp_refreshing());
            } else {
                /* Update current dirty area for next frame buffer */
                next_fb = flush_get_next_buf(lcd);
                flush_dirty_save(&dirty_area);
                if (! g_disable_sw_rotation) {
                    flush_dirty_copy(next_fb, color_map, &dirty_area);
                } else {
                    // direct partial copy assuming no rotation is required
                    lv_coord_t x_start, x_end, y_start, y_end;
                    for (int i = 0; i < dirty_area.inv_p; ++i) {
                        if (dirty_area.inv_area_joined[i] == 0) {
                            x_start               = dirty_area.inv_areas[i].x1;
                            x_end                 = dirty_area.inv_areas[i].x2;
                            y_start               = dirty_area.inv_areas[i].y1;
                            y_end                 = dirty_area.inv_areas[i].y2;
                            size_t bytes_per_line = (x_end - x_start + 1) * sizeof(lv_color_t);
                            for (int yy = y_start; yy <= y_end; yy++) {
                                uint8_t* s = ((uint8_t*)color_map) +
                                             (yy - offsety1) * (offsetx2 - offsetx1 + 1) * sizeof(lv_color_t) +
                                             (x_start - offsetx1) * sizeof(lv_color_t);
                                uint8_t* d = ((uint8_t*)next_fb) + yy * (LV_HOR_RES * sizeof(lv_color_t)) +
                                             x_start * sizeof(lv_color_t);
                                memcpy(d, s, bytes_per_line);
                            }
                        }
                    }
                }
                /* Switch the current LCD frame buffer to `next_fb` */
                lcd->switchFrameBufferTo(next_fb);
                /* Wait for the current frame buffer to complete transmission before reusing LVGL buffers. */
                ulTaskNotifyValueClear(NULL, ULONG_MAX);
                ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                if (probe_result == FLUSH_PROBE_PART_COPY) {
                    /* Synchronously update the dirty area for another frame
                     * buffer (non-blocking) */
                    flush_dirty_save(&dirty_area);
                    if (! g_disable_sw_rotation) {
                        flush_dirty_copy(flush_get_next_buf(lcd), color_map, &dirty_area);
                    } else {
                        // best-effort direct copy (omitted - already handled above)
                    }
                    flush_get_next_buf(lcd);
                }
            }
        }
    }
    lv_disp_flush_ready(drv);
}
#else
static void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map)
{
    LCD* lcd = (LCD*)drv->user_data;
    /* Action after last area refresh */
    if (lv_disp_flush_is_last(drv)) {
        /* Switch the current LCD frame buffer to `color_map` */
        if (! g_disable_sw_rotation) {
            lcd->switchFrameBufferTo(color_map);
        } else {
            // SW-rotation disabled: assume buffer orientation matches and switch
            lcd->switchFrameBufferTo(color_map);
        }
        /* Wait for the last frame buffer to complete transmission. */
        ulTaskNotifyValueClear(NULL, ULONG_MAX);
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    lv_disp_flush_ready(drv);
}
#endif /* LVGL_PORT_ROTATION_DEGREE */
#elif LVGL_PORT_FULL_REFRESH && LVGL_PORT_DISP_BUFFER_NUM == 2
static void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map)
{
    LCD* lcd = (LCD*)drv->user_data;
    /* Switch the current LCD frame buffer to `color_map` */
    lcd->switchFrameBufferTo(color_map);
    /* Wait for the last frame buffer to complete transmission. */
    ulTaskNotifyValueClear(NULL, ULONG_MAX);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    lv_disp_flush_ready(drv);
}
#elif LVGL_PORT_FULL_REFRESH && LVGL_PORT_DISP_BUFFER_NUM == 3
#if LVGL_PORT_ROTATION_DEGREE == 0
static void* lvgl_port_lcd_last_buf   = NULL;
static void* lvgl_port_lcd_next_buf   = NULL;
static void* lvgl_port_flush_next_buf = NULL;
#endif
void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map)
{
    LCD* lcd = (LCD*)drv->user_data;
#if LVGL_PORT_ROTATION_DEGREE != 0
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    void* next_fb      = get_next_frame_buffer(lcd);
    /* Rotate and copy dirty area from the current LVGL's buffer to the next LCD
     * frame buffer */
    if (! g_disable_sw_rotation) {
        rotate_copy_pixel((uint8_t*)color_map, (uint8_t*)next_fb, offsetx1, offsety1, offsetx2, offsety2, LV_HOR_RES,
                          LV_VER_RES, LVGL_PORT_ROTATION_DEGREE);
    } else {
        // direct copy (assume orientation match)
        size_t bytes_per_line = (offsetx2 - offsetx1 + 1) * sizeof(lv_color_t);
        for (int yy = offsety1; yy <= offsety2; yy++) {
            uint8_t* s = ((uint8_t*)color_map) + (yy - offsety1) * bytes_per_line;
            uint8_t* d = ((uint8_t*)next_fb) + yy * (LV_HOR_RES * sizeof(lv_color_t)) + offsetx1 * sizeof(lv_color_t);
            memcpy(d, s, bytes_per_line);
        }
    }
    /* Switch the current LCD frame buffer to `next_fb` */
    lcd->switchFrameBufferTo(next_fb);
#else
    drv->draw_buf->buf1      = color_map;
    drv->draw_buf->buf2      = lvgl_port_flush_next_buf;
    lvgl_port_flush_next_buf = color_map;
    /* Switch the current LCD frame buffer to `color_map` */
    lcd->switchFrameBufferTo(color_map);
    lvgl_port_lcd_next_buf = color_map;
#endif
    /* For triple buffer mode we can accept that buffer switching does not block
       here; the LCD vsync handler will manage the final buffer rotation. */
    lv_disp_flush_ready(drv);
}
#endif

IRAM_ATTR bool onLcdVsyncCallback(void* user_data)
{
    BaseType_t need_yield = pdFALSE;
#if LVGL_PORT_FULL_REFRESH && (LVGL_PORT_DISP_BUFFER_NUM == 3) && (LVGL_PORT_ROTATION_DEGREE == 0)
    if (lvgl_port_lcd_next_buf != lvgl_port_lcd_last_buf) {
        lvgl_port_flush_next_buf = lvgl_port_lcd_last_buf;
        lvgl_port_lcd_last_buf   = lvgl_port_lcd_next_buf;
    }
#else
    TaskHandle_t task_handle = (TaskHandle_t)user_data;
    // Notify that the current LCD frame buffer has been transmitted
    xTaskNotifyFromISR(task_handle, ULONG_MAX, eNoAction, &need_yield);
#endif
    return (need_yield == pdTRUE);
}
#else
void flush_callback(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* color_map)
{
    LCD* lcd           = (LCD*)drv->user_data;
    const int offsetx1 = area->x1;
    const int offsetx2 = area->x2;
    const int offsety1 = area->y1;
    const int offsety2 = area->y2;
    // Use drawBitmap (driver-dependent). We treat it as non-blocking for LVGL
    lcd->drawBitmap(offsetx1, offsety1, offsetx2 - offsetx1 + 1, offsety2 - offsety1 + 1, (const uint8_t*)color_map);
    // For RGB LCD, directly notify LVGL that the buffer is ready
    if (lcd->getBus()->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
        lv_disp_flush_ready(drv);
    }
}

static void update_callback(lv_disp_drv_t* drv)
{
    LCD* lcd                       = (LCD*)drv->user_data;
    auto transformation            = lcd->getTransformation();
    static bool disp_init_mirror_x = transformation.mirror_x;
    static bool disp_init_mirror_y = transformation.mirror_y;
    static bool disp_init_swap_xy  = transformation.swap_xy;
    switch (drv->rotated) {
    case LV_DISP_ROT_NONE:
        lcd->swapXY(disp_init_swap_xy);
        lcd->mirrorX(disp_init_mirror_x);
        lcd->mirrorY(disp_init_mirror_y);
        break;
    case LV_DISP_ROT_90:
        lcd->swapXY(! disp_init_swap_xy);
        lcd->mirrorX(disp_init_mirror_x);
        lcd->mirrorY(! disp_init_mirror_y);
        break;
    case LV_DISP_ROT_180:
        lcd->swapXY(disp_init_swap_xy);
        lcd->mirrorX(! disp_init_mirror_x);
        lcd->mirrorY(! disp_init_mirror_y);
        break;
    case LV_DISP_ROT_270:
        lcd->swapXY(! disp_init_swap_xy);
        lcd->mirrorX(! disp_init_mirror_x);
        lcd->mirrorY(disp_init_mirror_y);
        break;
    }
    ESP_UTILS_LOGD("Update display rotation to %d", drv->rotated);
#if ESP_UTILS_CONF_LOG_LEVEL == ESP_UTILS_LOG_LEVEL_DEBUG
    transformation     = lcd->getTransformation();
    disp_init_mirror_x = transformation.mirror_x;
    disp_init_mirror_y = transformation.mirror_y;
    disp_init_swap_xy  = transformation.swap_xy;
    ESP_UTILS_LOGD("Current mirror x: %d, mirror y: %d, swap xy: %d", disp_init_mirror_x, disp_init_mirror_y,
                   disp_init_swap_xy);
#endif
}
#endif /* LVGL_PORT_AVOID_TEAR */

void rounder_callback(lv_disp_drv_t* drv, lv_area_t* area)
{
    LCD* lcd        = (LCD*)drv->user_data;
    uint8_t x_align = lcd->getBasicAttributes().basic_bus_spec.x_coord_align;
    uint8_t y_align = lcd->getBasicAttributes().basic_bus_spec.y_coord_align;
    if (x_align > 1) {
        // round the start of coordinate down to the nearest aligned value
        area->x1 &= ~(x_align - 1);
        // round the end of coordinate up to the nearest aligned value
        area->x2 = (area->x2 & ~(x_align - 1)) + x_align - 1;
    }
    if (y_align > 1) {
        // round the start of coordinate down to the nearest aligned value
        area->y1 &= ~(y_align - 1);
        // round the end of coordinate up to the nearest aligned value
        area->y2 = (area->y2 & ~(y_align - 1)) + y_align - 1;
    }
}

static lv_disp_t* display_init(LCD* lcd)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lcd != nullptr, nullptr, "Invalid LCD device");
    ESP_UTILS_CHECK_FALSE_RETURN(lcd->getRefreshPanelHandle() != nullptr, nullptr, "LCD device is not initialized");
    static lv_disp_draw_buf_t disp_buf;
    static lv_disp_drv_t disp_drv;
    // Alloc draw buffers used by LVGL
    auto lcd_width  = lcd->getFrameWidth();
    auto lcd_height = lcd->getFrameHeight();
    int buffer_size = 0;
    int active_buffer_count = LVGL_PORT_BUFFER_NUM;
    ESP_UTILS_LOGI("Attempting aggressive allocation for LVGL draw buffers (full-frame preferred)");
#if ! LVGL_PORT_AVOID_TEAR
    // Avoid tearing function is disabled -> we must create LVGL draw buffers manually
    // Try full-frame allocation first (2 * full frame)
    size_t full_frame_bytes = (size_t)lcd_width * (size_t)lcd_height * sizeof(lv_color_t);
    bool allocated          = false;
    // Try to allocate two full-frame buffers in preferred caps
    for (int attempt = 0; attempt < 3 && ! allocated; ++attempt) {
        if (attempt == 0) {
            // full-frame attempt
            buffer_size = lcd_width * lcd_height;
            ESP_UTILS_LOGI("Trying full-frame buffers (size=%d pixels, bytes=%" PRIu32 ")", buffer_size,
                           (uint32_t)full_frame_bytes);
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                lvgl_buf[i] = heap_caps_malloc(full_frame_bytes, LVGL_PORT_BUFFER_MALLOC_CAPS);
                if (! lvgl_buf[i])
                    break;
                ESP_UTILS_LOGD("Full buffer[%d] addr: %p", i, lvgl_buf[i]);
            }
            // verify all allocated
            bool all_ok = true;
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                if (! lvgl_buf[i])
                    all_ok = false;
            }
            if (! all_ok) {
                // free any partially allocated
                for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                    if (lvgl_buf[i]) {
                        free(lvgl_buf[i]);
                        lvgl_buf[i] = nullptr;
                    }
                }
            } else
                allocated = true;
            if (allocated) {
                active_buffer_count = LVGL_PORT_BUFFER_NUM;
            }
            if (! allocated)
                ESP_UTILS_LOGW("Full-frame allocation failed, will fallback");
        } else if (attempt == 1) {
            // try half-frame (height/2)
            int h        = (lcd_height + 1) / 2;
            buffer_size  = lcd_width * h;
            size_t bytes = (size_t)buffer_size * sizeof(lv_color_t);
            ESP_UTILS_LOGI("Trying half-frame buffers (height=%d, bytes=%" PRIu32 ")", h, (uint32_t)bytes);
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                lvgl_buf[i] = heap_caps_malloc(bytes, LVGL_PORT_BUFFER_MALLOC_CAPS);
                if (! lvgl_buf[i])
                    break;
                ESP_UTILS_LOGD("Half buffer[%d] addr: %p", i, lvgl_buf[i]);
            }
            bool all_ok = true;
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                if (! lvgl_buf[i])
                    all_ok = false;
            }
            if (! all_ok) {
                for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                    if (lvgl_buf[i]) {
                        free(lvgl_buf[i]);
                        lvgl_buf[i] = nullptr;
                    }
                }
            } else
                allocated = true;
            if (allocated) {
                active_buffer_count = LVGL_PORT_BUFFER_NUM;
            }
            if (! allocated)
                ESP_UTILS_LOGW("Half-frame allocation failed, will fallback");
        } else {
            // try quarter-frame (height/4)
            int h        = (lcd_height + 3) / 4;
            buffer_size  = lcd_width * h;
            size_t bytes = (size_t)buffer_size * sizeof(lv_color_t);
            ESP_UTILS_LOGI("Trying quarter-frame buffers (height=%d, bytes=%" PRIu32 ")", h, (uint32_t)bytes);
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                lvgl_buf[i] = heap_caps_malloc(bytes, LVGL_PORT_BUFFER_MALLOC_CAPS);
                if (! lvgl_buf[i])
                    break;
                ESP_UTILS_LOGD("Quarter buffer[%d] addr: %p", i, lvgl_buf[i]);
            }
            bool all_ok = true;
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                if (! lvgl_buf[i])
                    all_ok = false;
            }
            if (! all_ok) {
                for (int i = 0; i < LVGL_PORT_BUFFER_NUM && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                    if (lvgl_buf[i]) {
                        free(lvgl_buf[i]);
                        lvgl_buf[i] = nullptr;
                    }
                }
            } else
                allocated = true;
            if (allocated) {
                active_buffer_count = LVGL_PORT_BUFFER_NUM;
            }
            if (! allocated)
                ESP_UTILS_LOGW(
                    "Quarter-frame allocation failed — will fall back to configured LVGL_PORT_BUFFER_SIZE_HEIGHT");
        }
    }
    if (! allocated) {
        // Final fallback path: choose the best safe draw-buffer layout for current internal RAM pressure.
        const size_t line_bytes = (size_t)lcd_width * sizeof(lv_color_t);
        const size_t largest_block_bytes = heap_caps_get_largest_free_block(LVGL_PORT_BUFFER_MALLOC_CAPS);
        const size_t free_total_bytes = heap_caps_get_free_size(LVGL_PORT_BUFFER_MALLOC_CAPS);
        constexpr size_t kInternalReserveBytes = 176 * 1024;
        constexpr int kMaxDualFallbackHeight = 16;
        constexpr int kMaxSingleFallbackHeight = 32;

        auto clear_lvgl_buffers = [&]() {
            for (int i = 0; i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                if (lvgl_buf[i]) {
                    free(lvgl_buf[i]);
                    lvgl_buf[i] = nullptr;
                }
            }
        };

        auto try_alloc_height = [&](int draw_buf_count, int h) -> bool {
            if (h < LVGL_PORT_BUFFER_SIZE_HEIGHT) {
                return false;
            }
            clear_lvgl_buffers();
            buffer_size = lcd_width * h;
            const size_t bytes = (size_t)buffer_size * sizeof(lv_color_t);
            for (int i = 0; i < draw_buf_count && i < LVGL_PORT_BUFFER_NUM_MAX; ++i) {
                lvgl_buf[i] = heap_caps_malloc(bytes, LVGL_PORT_BUFFER_MALLOC_CAPS);
                if (!lvgl_buf[i]) {
                    clear_lvgl_buffers();
                    return false;
                }
            }
            active_buffer_count = draw_buf_count;
            return true;
        };

        auto calc_max_height = [&](int draw_buf_count, int hard_cap) -> int {
            size_t budget_per_buffer_bytes = 0;
            if (free_total_bytes > kInternalReserveBytes) {
                budget_per_buffer_bytes = (free_total_bytes - kInternalReserveBytes) / (size_t)draw_buf_count;
            }
            size_t max_bytes_by_limits = largest_block_bytes;
            if (budget_per_buffer_bytes > 0 && budget_per_buffer_bytes < max_bytes_by_limits) {
                max_bytes_by_limits = budget_per_buffer_bytes;
            }
            int max_h = LVGL_PORT_BUFFER_SIZE_HEIGHT;
            if (max_bytes_by_limits >= line_bytes) {
                max_h = (int)(max_bytes_by_limits / line_bytes);
            }
            if (max_h > hard_cap) {
                max_h = hard_cap;
            }
            if (max_h > (lcd_height / 2)) {
                max_h = lcd_height / 2;
            }
            if (max_h < LVGL_PORT_BUFFER_SIZE_HEIGHT) {
                max_h = LVGL_PORT_BUFFER_SIZE_HEIGHT;
            }
            return max_h;
        };

        const int dual_candidates[] = {16, 14, 12, 10};
        const int single_candidates[] = {32, 28, 24, 20, 16, 12, 10};

        const int max_dual_h = calc_max_height(2, kMaxDualFallbackHeight);
        const int max_single_h = calc_max_height(1, kMaxSingleFallbackHeight);

        bool fallback_ok = false;
        int selected_height = 0;

        // If dual buffers would be too short, prefer one larger buffer to reduce visible artifacts.
        const bool prefer_single = (max_dual_h <= 12);
        const int* first_candidates = prefer_single ? single_candidates : dual_candidates;
        const size_t first_candidates_count = prefer_single
                                                  ? (sizeof(single_candidates) / sizeof(single_candidates[0]))
                                                  : (sizeof(dual_candidates) / sizeof(dual_candidates[0]));
        const int first_count = prefer_single ? 1 : 2;
        const int first_max_h = prefer_single ? max_single_h : max_dual_h;

        for (size_t i = 0; i < first_candidates_count; ++i) {
            int h = first_candidates[i];
            if (h > first_max_h) {
                continue;
            }
            if (try_alloc_height(first_count, h)) {
                fallback_ok = true;
                selected_height = h;
                break;
            }
        }

        if (!fallback_ok) {
            const int* second_candidates = prefer_single ? dual_candidates : single_candidates;
            const size_t second_candidates_count = prefer_single
                                                       ? (sizeof(dual_candidates) / sizeof(dual_candidates[0]))
                                                       : (sizeof(single_candidates) / sizeof(single_candidates[0]));
            const int second_count = prefer_single ? 2 : 1;
            const int second_max_h = prefer_single ? max_dual_h : max_single_h;
            for (size_t i = 0; i < second_candidates_count; ++i) {
                int h = second_candidates[i];
                if (h > second_max_h) {
                    continue;
                }
                if (try_alloc_height(second_count, h)) {
                    fallback_ok = true;
                    selected_height = h;
                    break;
                }
            }
        }

        if (! fallback_ok) {
            ESP_UTILS_LOGE("Fallback draw-buffer allocation failed");
            return nullptr;
        }

        ESP_UTILS_LOGI(
            "Fallback draw-buffer used: buffers=%d height=%d (largest=%" PRIu32 "B free=%" PRIu32 "B reserve=%" PRIu32
            "B max_dual=%d max_single=%d)",
            active_buffer_count,
            selected_height,
            (uint32_t)largest_block_bytes,
            (uint32_t)free_total_bytes,
            (uint32_t)kInternalReserveBytes,
            max_dual_h,
            max_single_h
        );
    } else {
        ESP_UTILS_LOGI("Aggressive draw-buffer allocation succeeded: buffer_size(pixels)=%d", buffer_size);
    }
#else
    // To avoid the tearing effect, we should use at least two frame buffers:
    buffer_size = lcd_width * lcd_height;
#if (LVGL_PORT_DISP_BUFFER_NUM >= 3) && (LVGL_PORT_ROTATION_DEGREE == 0) && LVGL_PORT_FULL_REFRESH
    lvgl_port_lcd_last_buf   = lcd->getFrameBufferByIndex(0);
    lvgl_buf[0]              = lcd->getFrameBufferByIndex(1);
    lvgl_buf[1]              = lcd->getFrameBufferByIndex(2);
    lvgl_port_lcd_next_buf   = lvgl_port_lcd_last_buf;
    lvgl_port_flush_next_buf = lvgl_buf[1];
    active_buffer_count      = 2;
#elif (LVGL_PORT_DISP_BUFFER_NUM >= 3) && (LVGL_PORT_ROTATION_DEGREE != 0)
    lvgl_buf[0] = lcd->getFrameBufferByIndex(2);
    active_buffer_count = 1;
#elif LVGL_PORT_DISP_BUFFER_NUM >= 2
    for (int i = 0; (i < LVGL_PORT_DISP_BUFFER_NUM) && (i < LVGL_PORT_BUFFER_NUM_MAX); i++) {
        lvgl_buf[i] = lcd->getFrameBufferByIndex(i);
    }
    active_buffer_count = (LVGL_PORT_DISP_BUFFER_NUM >= 2) ? 2 : 1;
#endif
#endif /* LVGL_PORT_AVOID_TEAR */
    // initialize LVGL draw buffers
    void* second_buf = (active_buffer_count > 1) ? lvgl_buf[1] : nullptr;
    lv_disp_draw_buf_init(&disp_buf, lvgl_buf[0], second_buf, buffer_size);
    ESP_UTILS_LOGD("Register display driver to LVGL");
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = flush_callback;
#if (LVGL_PORT_ROTATION_DEGREE == 90) || (LVGL_PORT_ROTATION_DEGREE == 270)
    disp_drv.hor_res = lcd_height;
    disp_drv.ver_res = lcd_width;
#else
    disp_drv.hor_res = lcd_width;
    disp_drv.ver_res = lcd_height;
#endif
#if LVGL_PORT_AVOID_TEAR // Only available when the tearing effect is enabled
#if LVGL_PORT_FULL_REFRESH
    disp_drv.full_refresh = 1;
#elif LVGL_PORT_DIRECT_MODE
    disp_drv.direct_mode = 1;
#endif
#else  // Only available when the tearing effect is disabled
    // Prefer hardware rotation if available; otherwise disable sw_rotate
    if (lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_SWAP_XY) &&
        lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_MIRROR_X) &&
        lcd->getBasicAttributes().basic_bus_spec.isFunctionValid(LCD::BasicBusSpecification::FUNC_MIRROR_Y)) {
        disp_drv.drv_update_cb = update_callback;
        ESP_UTILS_LOGI("Hardware rotation available; using drv_update_cb");
    } else {
        disp_drv.sw_rotate = 0; // explicitly disable software rotate
        ESP_UTILS_LOGW(
            "Hardware rotation not available; software rotation disabled at runtime (g_disable_sw_rotation=true).");
    }
#endif /* LVGL_PORT_AVOID_TEAR */
    disp_drv.draw_buf  = &disp_buf;
    disp_drv.user_data = (void*)lcd;
    // Only available when the coordinate alignment is enabled
    if ((lcd->getBasicAttributes().basic_bus_spec.x_coord_align > 1) ||
        (lcd->getBasicAttributes().basic_bus_spec.y_coord_align > 1)) {
        disp_drv.rounder_cb = rounder_callback;
    }
    return lv_disp_drv_register(&disp_drv);
}

static void touch_poll_task_wrapper(void* arg) { touch_poll_task(arg); }

#if ! LV_TICK_CUSTOM
static void tick_increment(void* arg)
{
    /* Tell LVGL how many milliseconds have elapsed */
    lv_tick_inc(LVGL_PORT_TICK_PERIOD_MS);
}

static bool tick_init(void)
{
    // Tick interface for LVGL (using esp_timer to generate periodic event)
    const esp_timer_create_args_t lvgl_tick_timer_args = {.callback = &tick_increment, .name = "LVGL tick"};
    ESP_UTILS_CHECK_ERROR_RETURN(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer), false,
                                 "Create LVGL tick timer failed");
    ESP_UTILS_CHECK_ERROR_RETURN(esp_timer_start_periodic(lvgl_tick_timer, LVGL_PORT_TICK_PERIOD_MS * 1000), false,
                                 "Start LVGL tick timer failed");
    return true;
}

static bool tick_deinit(void)
{
    ESP_UTILS_CHECK_ERROR_RETURN(esp_timer_stop(lvgl_tick_timer), false, "Stop LVGL tick timer failed");
    ESP_UTILS_CHECK_ERROR_RETURN(esp_timer_delete(lvgl_tick_timer), false, "Delete LVGL tick timer failed");
    return true;
}
#endif

static void lvgl_port_task(void* arg)
{
    ESP_UTILS_LOGD("Starting LVGL task");
    uint32_t task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
    while (1) {
        if (lvgl_port_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            lvgl_port_unlock();
        }
        if (task_delay_ms > LVGL_PORT_TASK_MAX_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < LVGL_PORT_TASK_MIN_DELAY_MS) {
            task_delay_ms = LVGL_PORT_TASK_MIN_DELAY_MS;
        }
        // ensure small sleep to let lower-priority tasks run
        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}

IRAM_ATTR bool onDrawBitmapFinishCallback(void* user_data)
{
    lv_disp_drv_t* drv = (lv_disp_drv_t*)user_data;
    lv_disp_flush_ready(drv);
    return false;
}

bool lvgl_port_init(LCD* lcd, Touch* tp)
{
    ESP_UTILS_CHECK_FALSE_RETURN(lcd != nullptr, false, "Invalid LCD device");
    global_lcd_ptr = lcd;
    auto bus_type = lcd->getBus()->getBasicAttributes().type;
#if LVGL_PORT_AVOID_TEAR
    ESP_UTILS_CHECK_FALSE_RETURN((bus_type == ESP_PANEL_BUS_TYPE_RGB) || (bus_type == ESP_PANEL_BUS_TYPE_MIPI_DSI),
                                 false, "Avoid tearing function only works with RGB/MIPI-DSI LCD now");
    ESP_UTILS_LOGI("Avoid tearing is enabled, mode: %d, rotation: %d", LVGL_PORT_AVOID_TEARING_MODE,
                   LVGL_PORT_ROTATION_DEGREE);
#endif
    lv_disp_t* disp   = nullptr;
    lv_indev_t* indev = nullptr;
    /* Initialize LVGL core and tick */
    lv_init();
#if ! LV_TICK_CUSTOM
    ESP_UTILS_CHECK_FALSE_RETURN(tick_init(), false, "Initialize LVGL tick failed");
#endif
    ESP_UTILS_LOGI("Initializing LVGL display driver");
    disp = display_init(lcd);
    ESP_UTILS_CHECK_NULL_RETURN(disp, false, "Initialize LVGL display driver failed");
    // Record the initial rotation of the display
    lv_disp_set_rotation(disp, LV_DISP_ROT_NONE);
    // For non-RGB LCD, need to notify LVGL that the buffer is ready when the
    // refresh is finished. We only attach the callback later in lvgl_port_start()
    // when the lvgl task is running (to avoid races).
    if (bus_type != ESP_PANEL_BUS_TYPE_RGB) {
        ESP_UTILS_LOGD("Will attach refresh-finish callback in lvgl_port_start()");
    }
    if (tp != nullptr) {
        ESP_UTILS_LOGD("Initialize LVGL input driver");
        indev = indev_init(tp);
        ESP_UTILS_CHECK_NULL_RETURN(indev, false, "Initialize LVGL input driver failed");
#if LVGL_PORT_ROTATION_DEGREE != 0
        // we disabled SW rotation; ensure touch transformation via hardware if needed
        auto& transformation = tp->getTransformation();
        ESP_UTILS_LOGI("Touch transform flags: swap=%d mirrorX=%d mirrorY=%d", transformation.swap_xy,
                       transformation.mirror_x, transformation.mirror_y);
#endif
        // Start touch polling task (low-priority) to avoid blocking LVGL on touch I/O
        if (touch_poll_task_handle == nullptr) {
            BaseType_t prio = (tskIDLE_PRIORITY + 1);
            xTaskCreatePinnedToCore(touch_poll_task_wrapper, "touch_poll", 2048, (void*)tp, prio,
                                    &touch_poll_task_handle, tskNO_AFFINITY);
            ESP_UTILS_LOGI("Started touch polling task (handle=%p)", touch_poll_task_handle);
        }
    }
    ESP_UTILS_LOGD("Create mutex for LVGL");
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "Create LVGL mutex failed");
    /* IMPORTANT: do not start LVGL task here — lvgl_port_start() will create it,
       so user can call ui_init() before rendering starts. */
    ESP_UTILS_LOGI("LVGL initialized (task not started). Call lvgl_port_start() to start rendering.");
    return true;
}

bool lvgl_port_start(void)
{
    /* If already started — noop */
    if (lvgl_task_handle != NULL) {
        ESP_UTILS_LOGW("lvgl_port_start: LVGL task already running");
        return true;
    }
    ESP_UTILS_LOGD("Create LVGL task");
    BaseType_t core_id   = (LVGL_PORT_TASK_CORE < 0) ? tskNO_AFFINITY : LVGL_PORT_TASK_CORE;
    BaseType_t lvgl_prio = LVGL_PORT_TASK_PRIORITY;
    BaseType_t ret       = xTaskCreatePinnedToCore(lvgl_port_task, "lvgl", LVGL_PORT_TASK_STACK_SIZE, NULL, lvgl_prio,
                                                   &lvgl_task_handle, core_id);
    ESP_UTILS_CHECK_FALSE_RETURN(ret == pdPASS, false, "Create LVGL task failed");
#if LVGL_PORT_AVOID_TEAR
    /* For non-RGB (or depending on bus_type) — attach refresh finish callback */
    if (global_lcd_ptr != nullptr) {
        global_lcd_ptr->attachRefreshFinishCallback(onLcdVsyncCallback, (void*)lvgl_task_handle);
    } else {
        ESP_UTILS_LOGW("lvgl_port_start: global_lcd_ptr is null, cannot attach refresh finish callback");
    }
#endif
    ESP_UTILS_LOGI("LVGL task started at max FreeRTOS priority (prio=%d of max=%d)", lvgl_prio,
                   configMAX_PRIORITIES - 1);
    return true;
}

bool lvgl_port_lock(int timeout_ms)
{
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "LVGL mutex is not initialized");
    const TickType_t timeout_ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return (xSemaphoreTakeRecursive(lvgl_mux, timeout_ticks) == pdTRUE);
}

bool lvgl_port_unlock(void)
{
    ESP_UTILS_CHECK_NULL_RETURN(lvgl_mux, false, "LVGL mutex is not initialized");
    xSemaphoreGiveRecursive(lvgl_mux);
    return true;
}

bool lvgl_port_deinit(void)
{
#if ! LV_TICK_CUSTOM
    ESP_UTILS_CHECK_FALSE_RETURN(tick_deinit(), false, "Deinitialize LVGL tick failed");
#endif
    ESP_UTILS_CHECK_FALSE_RETURN(lvgl_port_lock(-1), false, "Lock LVGL failed");
    if (lvgl_task_handle != nullptr) {
        vTaskDelete(lvgl_task_handle);
        lvgl_task_handle = nullptr;
    }
    ESP_UTILS_CHECK_FALSE_RETURN(lvgl_port_unlock(), false, "Unlock LVGL failed");
#if LV_ENABLE_GC || ! LV_MEM_CUSTOM
    lv_deinit();
#else
    ESP_UTILS_LOGW("LVGL memory is custom, `lv_deinit()` will not work");
#endif
#if ! LVGL_PORT_AVOID_TEAR
    for (int i = 0; i < LVGL_PORT_BUFFER_NUM; i++) {
        if (lvgl_buf[i] != nullptr) {
            free(lvgl_buf[i]);
            lvgl_buf[i] = nullptr;
        }
    }
#endif
    if (touch_poll_task_handle) {
        vTaskDelete(touch_poll_task_handle);
        touch_poll_task_handle = nullptr;
    }
    if (lvgl_mux != nullptr) {
        vSemaphoreDelete(lvgl_mux);
        lvgl_mux = nullptr;
    }
    global_lcd_ptr = nullptr;
    return true;
}
