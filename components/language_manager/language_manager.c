#include "language_manager.h"
#include "nvs.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

#define MAX_LANG_CBS 8
#define LANG_NAMESPACE "ui_prefs"
#define LANG_KEY_EN "lang_en"

static const char *TAG = "language_manager";

static bool g_lang_en = true;
static bool g_initialized = false;
static language_change_cb_t g_cbs[MAX_LANG_CBS];
static void *g_ctxs[MAX_LANG_CBS];

static void language_save_to_nvs(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(LANG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed while saving language: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(handle, LANG_KEY_EN, g_lang_en ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist language: %s", esp_err_to_name(err));
    }

    nvs_close(handle);
}

void language_init(bool default_en)
{
    if (g_initialized) {
        return;
    }

    g_lang_en = default_en;
    for (int i = 0; i < MAX_LANG_CBS; ++i) {
        g_cbs[i] = NULL;
        g_ctxs[i] = NULL;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(LANG_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t stored = default_en ? 1 : 0;
        err = nvs_get_u8(handle, LANG_KEY_EN, &stored);
        if (err == ESP_OK) {
            g_lang_en = (stored != 0);
            ESP_LOGI(TAG, "Loaded persisted language: %s", g_lang_en ? "EN" : "ZH");
        }
        nvs_close(handle);
    }

    g_initialized = true;
}

bool language_get_en(void) { return g_lang_en; }

void language_set_en(bool en)
{
    if (g_lang_en == en) return;
    g_lang_en = en;
    language_save_to_nvs();
    /* notify callbacks */
    for (int i = 0; i < MAX_LANG_CBS; ++i) {
        if (g_cbs[i]) g_cbs[i](g_lang_en, g_ctxs[i]);
    }
}

void language_toggle(void)
{
    language_set_en(!g_lang_en);
}

int language_register_callback(language_change_cb_t cb, void *ctx)
{
    if (!cb) return -1;
    for (int i = 0; i < MAX_LANG_CBS; ++i) {
        if (g_cbs[i] == NULL) {
            g_cbs[i] = cb;
            g_ctxs[i] = ctx;
            /* upon register, immediately call with current language so caller can init */
            cb(g_lang_en, ctx);
            return i + 1;
        }
    }
    return -1;
}

void language_unregister_callback(int id)
{
    if (id <= 0) return;
    int idx = id - 1;
    if (idx < 0 || idx >= MAX_LANG_CBS) return;
    g_cbs[idx] = NULL;
    g_ctxs[idx] = NULL;
}
