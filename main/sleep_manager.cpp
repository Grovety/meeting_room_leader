#include "sleep_manager.h"

#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "i2c_master.h"
#include "lvgl.h"
#include "lvgl_v8_port.h"
#include "nvs.h"

using namespace esp_panel::board;

extern "C" {
#include "ui.h"
}

static const char* TAG = "sleep_manager";

static Board* s_board  = nullptr;
static bool s_is_sleep = false;

static lv_obj_t* s_sleep_overlay                  = nullptr;
static lv_timer_t* s_overlay_remove_timer         = nullptr;
static lv_timer_t* s_backlight_fade_timer         = nullptr;
static int s_fade_target                          = 0;
static int s_fade_step                            = 0;
static int s_current_fade_brightness              = 0;
static int s_last_brightness                      = 100;
static int s_backlight_0x30_available             = -1;
static esp_io_expander_handle_t s_manual_expander = nullptr;
static int64_t s_sleep_entered_at_us              = 0;

static const uint32_t kDefaultSleepTimeoutMs   = 3 * 60 * 1000;
static const uint32_t kMinSleepTimeoutMs       = 1 * 60 * 1000;
static const uint32_t kMaxSleepTimeoutMs       = 12 * 60 * 60 * 1000;
static const int64_t kSleepWakeActivityGraceUs = 1000 * 1000LL;
static const char* const kSleepNvsNamespace    = "sleep_mgr";
static const char* const kSleepTimeoutKey      = "timeout_ms";
static uint32_t s_sleep_timeout_ms             = kDefaultSleepTimeoutMs;

#define I2C_PORT_NUM          I2C_NUM_0
#define I2C_MASTER_TIMEOUT_MS 1000
#define BACKLIGHT_ADDR_V1_1   0x30
#define BACKLIGHT_CMD_ON      0x10
#define BACKLIGHT_CMD_OFF     0x00
#define V1_0_BL_ON_MASK       ((uint32_t)((1U << 1) | (1U << 3) | (1U << 7)))
#define V1_0_BL_OFF_MASK      ((uint32_t)(1U << 1))

static void create_sleep_overlay(void);
static void remove_sleep_overlay_now(void);
static void overlay_event_cb(lv_event_t* e);
static void overlay_remove_timer_cb(lv_timer_t* t);
static void backlight_fade_timer_cb(lv_timer_t* t);
static void start_backlight_fade(Board* b, int duration_ms);
static uint32_t clamp_sleep_timeout_ms(uint32_t timeout_ms);
static void load_sleep_timeout_from_nvs(void);
static esp_err_t save_sleep_timeout_to_nvs(uint32_t timeout_ms);

static uint32_t clamp_sleep_timeout_ms(uint32_t timeout_ms)
{
    if (timeout_ms == 0)
        return 0;
    if (timeout_ms < kMinSleepTimeoutMs)
        return kMinSleepTimeoutMs;
    if (timeout_ms > kMaxSleepTimeoutMs)
        return kMaxSleepTimeoutMs;
    return timeout_ms;
}

static void load_sleep_timeout_from_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kSleepNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "nvs_open(%s) failed: %s", kSleepNvsNamespace, esp_err_to_name(err));
        }
        s_sleep_timeout_ms = kDefaultSleepTimeoutMs;
        return;
    }

    uint32_t stored_timeout_ms = kDefaultSleepTimeoutMs;
    err                        = nvs_get_u32(handle, kSleepTimeoutKey, &stored_timeout_ms);
    nvs_close(handle);
    if (err == ESP_OK) {
        s_sleep_timeout_ms = clamp_sleep_timeout_ms(stored_timeout_ms);
        ESP_LOGI(TAG, "Loaded sleep timeout: %lu ms", (unsigned long)s_sleep_timeout_ms);
        return;
    }

    if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "nvs_get_u32(%s) failed: %s", kSleepTimeoutKey, esp_err_to_name(err));
    }
    s_sleep_timeout_ms = kDefaultSleepTimeoutMs;
}

static esp_err_t save_sleep_timeout_to_nvs(uint32_t timeout_ms)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kSleepNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u32(handle, kSleepTimeoutKey, timeout_ms);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t native_i2c_write_byte(uint8_t dev_addr, uint8_t data)
{
    if (!i2c_master_lock(I2C_MASTER_TIMEOUT_MS)) {
        return ESP_ERR_TIMEOUT;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    if (! cmd) {
        i2c_master_unlock();
        return ESP_ERR_NO_MEM;
    }

    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (dev_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, data, true);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT_NUM, cmd, pdMS_TO_TICKS(I2C_MASTER_TIMEOUT_MS));
    i2c_cmd_link_delete(cmd);
    i2c_master_unlock();
    return ret;
}

static bool try_backlight_0x30_set(bool on)
{
    if (s_backlight_0x30_available == 0)
        return false;

    const uint8_t cmd = on ? BACKLIGHT_CMD_ON : BACKLIGHT_CMD_OFF;
    esp_err_t err     = native_i2c_write_byte(BACKLIGHT_ADDR_V1_1, cmd);
    if (err == ESP_OK) {
        if (s_backlight_0x30_available != 1) {
            ESP_LOGI(TAG, "Backlight 0x30 control is available");
        }
        s_backlight_0x30_available = 1;
        return true;
    }

    if (s_backlight_0x30_available < 0) {
        s_backlight_0x30_available = 0;
        ESP_LOGW(TAG, "Backlight 0x30 control unavailable: %s", esp_err_to_name(err));
    }
    return false;
}

static void expander_backlight_set(Board* b, int level)
{
    if (! b)
        return;

    auto exp = b->getIO_Expander();
    if (! exp)
        return;

    auto base = exp->getBase();
    if (! base)
        return;

#if defined(ESP_PANEL_BOARD_USE_BACKLIGHT)
    const int bl_io       = ESP_PANEL_BOARD_BACKLIGHT_IO;
    const int bl_on_level = ESP_PANEL_BOARD_BACKLIGHT_ON_LEVEL;
#else
    const int bl_io       = 1;
    const int bl_on_level = 1;
#endif

    if (!i2c_master_lock(1000)) {
        ESP_LOGW(TAG, "Failed to lock I2C for expander backlight update");
        return;
    }
    base->pinMode(bl_io, OUTPUT);
    base->digitalWrite(bl_io, level ? (bl_on_level ? 1 : 0) : (bl_on_level ? 0 : 1));
    i2c_master_unlock();
}

static void safe_backlight_set(Board* b, int brightness_percent)
{
    if (! b)
        return;

    int pct = brightness_percent;
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;

    if (pct > 0)
        s_last_brightness = pct;

    if (b->getIO_Expander()) {
        expander_backlight_set(b, pct > 0 ? 1 : 0);
        return;
    }

    if (s_manual_expander) {
        esp_err_t err = ESP_OK;
        if (!i2c_master_lock(1000)) {
            ESP_LOGW(TAG, "Failed to lock I2C for manual_expander backlight update");
            return;
        }
        if (pct > 0) {
            err = esp_io_expander_set_level(s_manual_expander, V1_0_BL_ON_MASK, 1);
        } else {
            err = esp_io_expander_set_level(s_manual_expander, V1_0_BL_OFF_MASK, 0);
        }
        i2c_master_unlock();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "manual_expander backlight set failed: %s", esp_err_to_name(err));
        }
        return;
    }

    auto bl = b->getBacklight();
    if (! bl) {
        if (! try_backlight_0x30_set(pct > 0)) {
            ESP_LOGW(TAG, "No backlight object/expander and 0x30 control unavailable");
        }
        return;
    }

    esp_err_t err = bl->setBrightness(pct);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Backlight setBrightness(%d) failed: %s", pct, esp_err_to_name(err));
    }
}

static void safe_display_set(Board* b, bool enable_on)
{
    (void)b;
    (void)enable_on;
}

static void start_backlight_fade(Board* b, int duration_ms)
{
    if (! b)
        return;

    if (b->getIO_Expander()) {
        safe_backlight_set(b, s_last_brightness);
        return;
    }

    auto bl = b->getBacklight();
    if (! bl) {
        safe_backlight_set(b, s_last_brightness);
        return;
    }

    if (s_backlight_fade_timer) {
        lv_timer_del(s_backlight_fade_timer);
        s_backlight_fade_timer = nullptr;
    }

    if (duration_ms <= 0) {
        safe_backlight_set(b, s_last_brightness);
        return;
    }

    int steps  = 8;
    int period = duration_ms / steps;
    if (period < 20) {
        period = 20;
        steps  = duration_ms / period;
        if (steps <= 0)
            steps = 1;
    }

    s_fade_target             = s_last_brightness;
    s_current_fade_brightness = 0;
    s_fade_step               = (s_fade_target + steps - 1) / steps;

    safe_backlight_set(b, 0);
    s_backlight_fade_timer = lv_timer_create(backlight_fade_timer_cb, period, b);
}

static void backlight_fade_timer_cb(lv_timer_t* t)
{
    Board* b = (Board*)t->user_data;
    if (! b) {
        if (s_backlight_fade_timer) {
            lv_timer_del(s_backlight_fade_timer);
            s_backlight_fade_timer = nullptr;
        }
        return;
    }

    s_current_fade_brightness += s_fade_step;
    if (s_current_fade_brightness >= s_fade_target) {
        safe_backlight_set(b, s_fade_target);
        if (s_backlight_fade_timer) {
            lv_timer_del(s_backlight_fade_timer);
            s_backlight_fade_timer = nullptr;
        }
        return;
    }

    safe_backlight_set(b, s_current_fade_brightness);
}

static void create_sleep_overlay(void)
{
    if (s_sleep_overlay)
        return;

    lv_obj_t* parent = lv_layer_top();
    if (! parent)
        parent = lv_scr_act();
    if (! parent) {
        ESP_LOGW(TAG, "create_sleep_overlay: no parent available");
        return;
    }

    s_sleep_overlay = lv_obj_create(parent);
    lv_obj_remove_style_all(s_sleep_overlay);

    lv_disp_t* disp = lv_disp_get_default();
    int hor         = disp ? lv_disp_get_hor_res(disp) : 480;
    int ver         = disp ? lv_disp_get_ver_res(disp) : 320;
    if (hor <= 0)
        hor = 480;
    if (ver <= 0)
        ver = 320;

    lv_obj_set_size(s_sleep_overlay, hor, ver);
    lv_obj_set_align(s_sleep_overlay, LV_ALIGN_CENTER);
    lv_obj_move_foreground(s_sleep_overlay);
    lv_obj_set_style_bg_color(s_sleep_overlay, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_sleep_overlay, 160, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_flag(s_sleep_overlay, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_ADV_HITTEST);
    lv_obj_clear_flag(s_sleep_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_sleep_overlay, overlay_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_sleep_overlay, overlay_event_cb, LV_EVENT_CLICKED, NULL);
}

static void remove_sleep_overlay_now(void)
{
    if (! s_sleep_overlay)
        return;
    lv_obj_del(s_sleep_overlay);
    s_sleep_overlay = nullptr;
}

static void overlay_remove_timer_cb(lv_timer_t* t)
{
    (void)t;
    if (s_overlay_remove_timer) {
        lv_timer_del(s_overlay_remove_timer);
        s_overlay_remove_timer = nullptr;
    }
    remove_sleep_overlay_now();
}

static void overlay_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_CLICKED && code != LV_EVENT_PRESSED && code != LV_EVENT_SHORT_CLICKED) {
        return;
    }

    if (s_overlay_remove_timer) {
        lv_timer_del(s_overlay_remove_timer);
        s_overlay_remove_timer = nullptr;
    }

    remove_sleep_overlay_now();
    sleep_manager_exit_sleep();
}

static void sleep_check_cb(lv_timer_t* timer)
{
    (void)timer;

    if (s_sleep_timeout_ms == 0) {
        return;
    }

    if (ui_is_booking_active()) {
        if (s_is_sleep) {
            ESP_LOGI(TAG, "Booking active -> exiting sleep to keep screen on");
            sleep_manager_exit_sleep();
        }
        return;
    }

    uint32_t inactive_ms = lv_disp_get_inactive_time(NULL);
    if (! s_is_sleep && inactive_ms >= s_sleep_timeout_ms) {
        sleep_manager_enter_sleep();
    } else if (s_is_sleep && inactive_ms <= 500) {
        const int64_t sleep_elapsed_us = esp_timer_get_time() - s_sleep_entered_at_us;
        if (sleep_elapsed_us > kSleepWakeActivityGraceUs) {
            ESP_LOGI(TAG, "Recent user activity while sleeping -> exiting sleep");
            sleep_manager_exit_sleep();
        }
    }
}

void sleep_manager_init(Board* board)
{
    s_board = board;
    load_sleep_timeout_from_nvs();
}

extern "C" void sleep_manager_set_manual_expander(esp_io_expander_handle_t expander) { s_manual_expander = expander; }

extern "C" void sleep_manager_start(void)
{
    lv_timer_t* t = lv_timer_create(sleep_check_cb, 500, NULL);
    if (t) {
        if (s_sleep_timeout_ms == 0) {
            ESP_LOGI(TAG, "Sleep timer created, auto sleep is disabled");
        } else {
            ESP_LOGI(TAG, "Sleep timer created, timeout=%lu ms", (unsigned long)s_sleep_timeout_ms);
        }
    }
}

extern "C" void sleep_manager_enter_sleep(void)
{
    if (s_is_sleep)
        return;
    s_is_sleep            = true;
    s_sleep_entered_at_us = esp_timer_get_time();

    ESP_LOGI(TAG, "Entering sleep mode (backlight off). Last brightness=%d", s_last_brightness);
    safe_backlight_set(s_board, 0);
    safe_display_set(s_board, false);
    create_sleep_overlay();
}

extern "C" void sleep_manager_exit_sleep(void)
{
    if (! s_is_sleep) {
        safe_display_set(s_board, true);
        safe_backlight_set(s_board, s_last_brightness);
        return;
    }

    s_is_sleep            = false;
    s_sleep_entered_at_us = 0;
    safe_display_set(s_board, true);
    start_backlight_fade(s_board, 240);

    if (s_sleep_overlay) {
        if (s_overlay_remove_timer) {
            lv_timer_del(s_overlay_remove_timer);
            s_overlay_remove_timer = nullptr;
        }
        s_overlay_remove_timer = lv_timer_create(overlay_remove_timer_cb, 80, NULL);
        if (s_overlay_remove_timer)
            lv_timer_set_repeat_count(s_overlay_remove_timer, 1);
        else
            remove_sleep_overlay_now();
    }
}

extern "C" void sleep_manager_reset_inactivity_timer(void)
{
    if (! lvgl_port_lock(-1)) {
        ESP_LOGW(TAG, "Failed to lock LVGL while resetting inactivity timer");
        return;
    }

    lv_disp_trig_activity(NULL);
    lvgl_port_unlock();
}

extern "C" bool sleep_manager_is_sleeping(void) { return s_is_sleep; }

extern "C" void sleep_manager_set_timeout_ms(uint32_t timeout_ms)
{
    const uint32_t clamped_timeout_ms = clamp_sleep_timeout_ms(timeout_ms);
    s_sleep_timeout_ms                = clamped_timeout_ms;

    esp_err_t err = save_sleep_timeout_to_nvs(clamped_timeout_ms);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to save sleep timeout: %s", esp_err_to_name(err));
    } else {
        if (clamped_timeout_ms == 0) {
            ESP_LOGI(TAG, "Sleep timeout saved: auto sleep disabled");
        } else {
            ESP_LOGI(TAG, "Sleep timeout saved: %lu ms", (unsigned long)clamped_timeout_ms);
        }
    }
}

extern "C" uint32_t sleep_manager_get_timeout_ms(void) { return s_sleep_timeout_ms; }
