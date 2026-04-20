#include "device_name_store.h"

#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "device_name";
static const char *NVS_NAMESPACE = "device_profile";
static const char *NVS_KEY_NAME = "device_name";
static const char *DEFAULT_DEVICE_NAME = "Meeting Room";

static char s_device_name[DEVICE_NAME_STORE_MAX_LEN + 1] = "Meeting Room";
static bool s_loaded = false;

static bool ascii_word_equal_ci(const char *a, size_t a_len, const char *b, size_t b_len)
{
    if (a_len != b_len) {
        return false;
    }

    for (size_t i = 0; i < a_len; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (tolower(ca) != tolower(cb)) {
            return false;
        }
    }

    return true;
}

static void sanitize_device_name(const char *input, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    const char *src = input ? input : "";
    while (*src && isspace((unsigned char)*src)) {
        ++src;
    }

    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len - 1])) {
        --len;
    }

    if (len == 0) {
        strlcpy(out, DEFAULT_DEVICE_NAME, out_size);
        return;
    }

    if (len > DEVICE_NAME_STORE_MAX_LEN) {
        len = DEVICE_NAME_STORE_MAX_LEN;
    }

    char normalized[DEVICE_NAME_STORE_MAX_LEN + 1];
    size_t norm_len = 0;
    bool prev_space = false;

    for (size_t i = 0; i < len && norm_len < DEVICE_NAME_STORE_MAX_LEN; ++i) {
        unsigned char ch = (unsigned char)src[i];
        bool is_space = isspace(ch);

        if (is_space) {
            if (norm_len == 0 || prev_space) {
                continue;
            }
            normalized[norm_len++] = ' ';
            prev_space = true;
            continue;
        }

        normalized[norm_len++] = (char)ch;
        prev_space = false;
    }

    while (norm_len > 0 && normalized[norm_len - 1] == ' ') {
        --norm_len;
    }

    normalized[norm_len] = '\0';

    size_t out_len = 0;
    const char *last_word = NULL;
    size_t last_word_len = 0;
    size_t idx = 0;

    while (idx < norm_len && out_len < DEVICE_NAME_STORE_MAX_LEN) {
        size_t word_start = idx;
        while (idx < norm_len && normalized[idx] != ' ') {
            ++idx;
        }
        size_t word_len = idx - word_start;
        bool duplicate_word =
            last_word && ascii_word_equal_ci(normalized + word_start, word_len, last_word, last_word_len);

        if (!duplicate_word && word_len > 0) {
            if (out_len > 0 && out_len < DEVICE_NAME_STORE_MAX_LEN) {
                out[out_len++] = ' ';
            }

            size_t copy_len = word_len;
            if (copy_len > DEVICE_NAME_STORE_MAX_LEN - out_len) {
                copy_len = DEVICE_NAME_STORE_MAX_LEN - out_len;
            }
            memcpy(out + out_len, normalized + word_start, copy_len);
            last_word = out + out_len;
            last_word_len = copy_len;
            out_len += copy_len;
        }

        while (idx < norm_len && normalized[idx] == ' ') {
            ++idx;
        }
    }

    if (out_len == 0) {
        strlcpy(out, DEFAULT_DEVICE_NAME, out_size);
        return;
    }

    out[out_len] = '\0';
}

static void load_device_name_once(void)
{
    if (s_loaded) {
        return;
    }

    s_loaded = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved device name in NVS (%s), using default", esp_err_to_name(err));
        strlcpy(s_device_name, DEFAULT_DEVICE_NAME, sizeof(s_device_name));
        return;
    }

    size_t required = 0;
    err = nvs_get_str(handle, NVS_KEY_NAME, NULL, &required);
    if (err == ESP_OK && required > 0) {
        char buf[DEVICE_NAME_STORE_MAX_LEN + 1];
        if (required > sizeof(buf)) {
            required = sizeof(buf);
        }
        err = nvs_get_str(handle, NVS_KEY_NAME, buf, &required);
        if (err == ESP_OK) {
            sanitize_device_name(buf, s_device_name, sizeof(s_device_name));
        } else {
            ESP_LOGW(TAG, "Failed to read device name from NVS: %s", esp_err_to_name(err));
            strlcpy(s_device_name, DEFAULT_DEVICE_NAME, sizeof(s_device_name));
        }
    } else {
        strlcpy(s_device_name, DEFAULT_DEVICE_NAME, sizeof(s_device_name));
    }

    nvs_close(handle);
}

const char *device_name_store_get(void)
{
    load_device_name_once();
    return s_device_name;
}

esp_err_t device_name_store_set(const char *name)
{
    char sanitized[DEVICE_NAME_STORE_MAX_LEN + 1];
    sanitize_device_name(name, sanitized, sizeof(sanitized));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_NAME, sanitized);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist device name: %s", esp_err_to_name(err));
        return err;
    }

    strlcpy(s_device_name, sanitized, sizeof(s_device_name));
    s_loaded = true;
    ESP_LOGI(TAG, "Device name saved: %s", s_device_name);
    return ESP_OK;
}
