#include "info_links_store.h"

#include <stdbool.h>
#include <ctype.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "info_links";
static const char *NVS_NAMESPACE = "info_links";
static const char *NVS_KEY_COMPANY = "company_url";
static const char *NVS_KEY_OFFICE = "office_map";
static const char *DEFAULT_COMPANY_WEBSITE_URL = "https://grovety.com/";
static const char *DEFAULT_OFFICE_MAP_URL = "https://grovety.com/demomap";

static info_links_store_data_t s_links = {
    .company_website = "https://grovety.com/",
    .office_map = "https://grovety.com/demomap",
};
static bool s_loaded = false;

static void copy_string(const char *src, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    if (!src) {
        src = "";
    }

    strlcpy(out, src, out_size);
}

static void sanitize_url(const char *input, char *out, size_t out_size)
{
    const char *src = input ? input : "";
    size_t len = 0;
    size_t start = 0;

    if (!out || out_size == 0) {
        return;
    }

    while (src[start] != '\0' && isspace((unsigned char)src[start])) {
        ++start;
    }

    len = strlen(src + start);
    while (len > 0 && isspace((unsigned char)src[start + len - 1])) {
        --len;
    }

    if (len > INFO_LINKS_STORE_MAX_URL_LEN) {
        len = INFO_LINKS_STORE_MAX_URL_LEN;
    }
    if (len > out_size - 1) {
        len = out_size - 1;
    }

    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
}

static void set_defaults(info_links_store_data_t *data)
{
    if (!data) {
        return;
    }

    copy_string(DEFAULT_COMPANY_WEBSITE_URL, data->company_website, sizeof(data->company_website));
    copy_string(DEFAULT_OFFICE_MAP_URL, data->office_map, sizeof(data->office_map));
}

static void load_value_or_keep(const char *key, char *out, size_t out_size, nvs_handle_t handle)
{
    esp_err_t err;
    size_t required = 0;

    if (!key || !out || out_size == 0) {
        return;
    }

    err = nvs_get_str(handle, key, NULL, &required);
    if (err != ESP_OK) {
        return;
    }

    if (required == 0) {
        out[0] = '\0';
        return;
    }

    char buffer[INFO_LINKS_STORE_MAX_URL_LEN + 1];
    if (required > sizeof(buffer)) {
        required = sizeof(buffer);
    }

    err = nvs_get_str(handle, key, buffer, &required);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read %s from NVS: %s", key, esp_err_to_name(err));
        return;
    }

    sanitize_url(buffer, out, out_size);
}

static void load_info_links_once(void)
{
    if (s_loaded) {
        return;
    }

    s_loaded = true;
    set_defaults(&s_links);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved Info links in NVS (%s), using defaults", esp_err_to_name(err));
        return;
    }

    load_value_or_keep(NVS_KEY_COMPANY, s_links.company_website, sizeof(s_links.company_website), handle);
    load_value_or_keep(NVS_KEY_OFFICE, s_links.office_map, sizeof(s_links.office_map), handle);
    nvs_close(handle);
}

void info_links_store_get(info_links_store_data_t *out)
{
    load_info_links_once();
    if (!out) {
        return;
    }

    memcpy(out, &s_links, sizeof(*out));
}

void info_links_store_get_company_website_copy(char *out, size_t out_size)
{
    load_info_links_once();
    copy_string(s_links.company_website, out, out_size);
}

void info_links_store_get_office_map_copy(char *out, size_t out_size)
{
    load_info_links_once();
    copy_string(s_links.office_map, out, out_size);
}

esp_err_t info_links_store_set(const info_links_store_data_t *data)
{
    info_links_store_data_t sanitized;
    nvs_handle_t handle;
    esp_err_t err;

    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(&sanitized, 0, sizeof(sanitized));
    sanitize_url(data->company_website, sanitized.company_website, sizeof(sanitized.company_website));
    sanitize_url(data->office_map, sanitized.office_map, sizeof(sanitized.office_map));

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(handle, NVS_KEY_COMPANY, sanitized.company_website);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, NVS_KEY_OFFICE, sanitized.office_map);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to persist Info links: %s", esp_err_to_name(err));
        return err;
    }

    memcpy(&s_links, &sanitized, sizeof(s_links));
    s_loaded = true;
    ESP_LOGI(TAG, "Info links updated");
    return ESP_OK;
}
