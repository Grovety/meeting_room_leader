#include "config_portal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "device_name_store.h"
#include "info_links_store.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "ui.h"
#include "weather_integration.h"
#include "wifi_manager.h"

static const char* TAG = "config_portal";

static const char* NVS_NAMESPACE     = "cfg_portal";
static const char* NVS_BOOT_ONCE_KEY = "boot_once";

static const char* AP_SSID_PREFIX = "Meeting Room-Config";
static const char* AP_PASSWORD    = "configure123";
static const char* AP_DEFAULT_URL = "http://192.168.4.1";

enum {
    DNS_PORT                = 53,
    HTTP_PORT               = 80,
    MAX_FORM_BODY           = 8192,
    MAX_ICAL_URL_LEN        = 512,
    MAX_PORTAL_SCAN_RESULTS = 20,
    MAX_PORTAL_SCAN_RECORDS = 40,
    POST_SAVE_DELAY_MS      = 2000,
};

typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} portal_scan_result_t;

static httpd_handle_t s_http_server   = NULL;
static TaskHandle_t s_dns_task        = NULL;
static int s_dns_sock                 = -1;
static bool s_dns_running             = false;
static esp_netif_t* s_ap_netif        = NULL;
static esp_netif_t* s_sta_netif       = NULL;
static bool s_portal_running          = false;
static bool s_runtime_mode            = false;
static config_portal_access_mode_t s_access_mode = CONFIG_PORTAL_ACCESS_NONE;
static bool s_runtime_auto_reconnect_prev = true;
static char s_ap_url[32]              = "http://192.168.4.1";
static char s_network_name[33]        = {0};
static SemaphoreHandle_t s_scan_mutex = NULL;
static SemaphoreHandle_t s_state_mutex = NULL;
static portal_scan_result_t s_scan_results[MAX_PORTAL_SCAN_RESULTS];
static size_t s_scan_result_count = 0;
static config_portal_phase_t s_phase = CONFIG_PORTAL_PHASE_IDLE;
static char s_status_text[128] = "Setup inactive";
static bool s_scan_refresh_task_running = false;
static bool s_runtime_apply_in_progress = false;
static char s_ap_ssid[33] = {0};

static lv_obj_t* s_screen = NULL;

static void ensure_ap_ssid(void)
{
    uint8_t mac[6]        = {0};
    const size_t suffix_len = 7; // "-AABBCC"
    size_t max_prefix_len;

    if (s_ap_ssid[0] != '\0') {
        return;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    max_prefix_len = sizeof(s_ap_ssid) - 1 - suffix_len;
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%.*s-%02X%02X%02X", (int)max_prefix_len, AP_SSID_PREFIX, mac[3], mac[4],
             mac[5]);
    s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
}

static void ensure_state_mutex(void)
{
    if (!s_state_mutex) {
        s_state_mutex = xSemaphoreCreateMutex();
    }
}

static void set_phase_status(config_portal_phase_t phase, const char* status)
{
    ensure_state_mutex();
    if (!s_state_mutex) {
        s_phase = phase;
        if (status) {
            strncpy(s_status_text, status, sizeof(s_status_text) - 1);
            s_status_text[sizeof(s_status_text) - 1] = '\0';
        }
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_phase = phase;
    if (status) {
        strncpy(s_status_text, status, sizeof(s_status_text) - 1);
        s_status_text[sizeof(s_status_text) - 1] = '\0';
    }
    xSemaphoreGive(s_state_mutex);
}

static bool runtime_apply_in_progress(void)
{
    bool in_progress = false;

    ensure_state_mutex();
    if (!s_state_mutex) {
        return s_runtime_apply_in_progress;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    in_progress = s_runtime_apply_in_progress;
    xSemaphoreGive(s_state_mutex);
    return in_progress;
}

static void set_runtime_apply_in_progress(bool in_progress)
{
    ensure_state_mutex();
    if (!s_state_mutex) {
        s_runtime_apply_in_progress = in_progress;
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_runtime_apply_in_progress = in_progress;
    xSemaphoreGive(s_state_mutex);
}

static void copy_connected_wifi_name(char* out, size_t out_size)
{
    wifi_ap_record_t ap_info = {0};

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (!wifi_manager_is_connected()) {
        return;
    }

    if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
        return;
    }

    strncpy(out, (const char*)ap_info.ssid, out_size - 1);
    out[out_size - 1] = '\0';
}

static const char* kWifiSetupPageTemplate =
    "<!doctype html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Meeting Room Setup</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#0f1218;--panel:#171c25;--muted:#98a2b3;--line:#273142;--accent:#3fb983;--accent2:#"
    "5ba6ff;}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;padding:24px;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe "
    "UI',sans-serif;color:#fff;"
    "background:radial-gradient(circle at top,#1a2330,#0b0d12 "
    "68%);display:flex;align-items:center;justify-content:center}"
    ".card{width:min(760px,100%%);background:rgba(23,28,37,.96);border:1px solid "
    "var(--line);border-radius:24px;padding:28px;"
    "box-shadow:0 24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:32px}"
    ".lead{margin:0 0 18px;color:var(--muted)}"
    ".ap{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:0 0 20px}"
    ".ap div{padding:14px 16px;border-radius:16px;background:#11161f;border:1px solid var(--line)}"
    ".ap "
    "small{display:block;margin-bottom:6px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:"
    "11px}"
    ".scan{display:grid;gap:12px;margin:0 0 22px}"
    ".scan h2{margin:0;font-size:20px}"
    ".scan p{margin:0;color:var(--muted);font-size:14px}"
    ".picker{padding:12px 14px;border-radius:14px;background:#10151d;border:1px solid "
    "var(--line);color:#dbe4f3;font-size:14px}"
    ".wifi-list{display:grid;gap:10px;max-height:280px;overflow:auto;padding-right:4px}"
    ".wifi-item{width:100%%;display:flex;align-items:center;justify-content:space-between;gap:14px;padding:14px "
    "16px;border-radius:16px;"
    "border:1px solid #2f3b4f;background:#10151d;color:#fff;text-align:left;cursor:pointer}"
    ".wifi-item:hover{border-color:#4a6287}"
    ".wifi-item.selected{border-color:var(--accent2);box-shadow:0 0 0 1px rgba(91,166,255,.32) inset}"
    ".wifi-name{display:block;font-weight:700}"
    ".wifi-meta{display:flex;align-items:center;justify-content:flex-end;gap:10px;flex-wrap:wrap;color:var(--muted);"
    "font-size:13px}"
    ".wifi-tag{display:inline-flex;align-items:center;padding:5px "
    "8px;border-radius:999px;background:rgba(63,185,131,.16);color:#d8ffe8;font-size:12px}"
    ".wifi-tag-open{background:rgba(91,166,255,.16);color:#dbe9ff}"
    ".scan-empty{padding:16px;border-radius:16px;background:#10151d;border:1px dashed #324055;color:var(--muted)}"
    "form{display:grid;gap:16px}"
    "label{display:grid;gap:8px;font-weight:600}"
    "input,textarea{width:100%%;border:1px solid #324055;border-radius:14px;background:#0f141c;color:#fff;padding:14px "
    "16px;font:inherit}"
    "textarea{min-height:120px;resize:vertical}"
    ".hint{margin-top:-6px;color:var(--muted);font-size:13px}"
    "button{border:0;border-radius:14px;padding:14px "
    "18px;font:inherit;font-weight:700;cursor:pointer;background:linear-gradient(135deg,var(--accent),var(--accent2));"
    "color:#081018}"
    ".foot{margin-top:16px;color:var(--muted);font-size:13px}"
    "@media(max-width:640px){body{padding:14px}.card{padding:18px;border-radius:20px}h1{font-size:26px}.wifi-item{"
    "align-items:flex-start;flex-direction:column}.wifi-meta{justify-content:flex-start}}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>Meeting Room Setup Mode</h1>"
    "<p class='lead'>Connect to the board Wi-Fi, enter the working Wi-Fi credentials, then save. After the board "
    "reconnects, reopen setup from the board to add Info screen QR links.</p>"
    "<div class='ap'>"
    "<div><small>Access Point</small><strong>%s</strong></div>"
    "<div><small>Password</small><strong>%s</strong></div>"
    "<div><small>Open</small><strong>%s</strong></div>"
    "</div>"
    "<div class='scan'>"
    "<h2>Nearby Wi-Fi Networks</h2>"
    "<p>Tap a visible network to fill the SSID. Protected networks will prompt for the password.</p>"
    "<div class='picker' id='selectedNote'>Tap a network below or enter the SSID manually.</div>"
    "<div class='wifi-list'>%s</div>"
    "</div>"
    "<form method='POST' action='/save'>"
    "<label>Wi-Fi SSID"
    "<input type='text' name='wifi_ssid' maxlength='32' required value='%s'>"
    "</label>"
    "<label>Wi-Fi Password"
    "<input type='password' name='wifi_pass' maxlength='64' value=''>"
    "</label>"
    "<button type='submit'>Save Settings</button>"
    "</form>"
    "<div class='foot'>This step saves only the Wi-Fi connection. The Info screen QR links are "
    "configured on the next page.</div>"
    "<script>"
    "function selectNetwork(btn){"
    "var ssid=btn.getAttribute('data-ssid')||'';"
    "var secure=btn.getAttribute('data-secure')==='1';"
    "var ssidInput=document.querySelector(\"input[name='wifi_ssid']\");"
    "var passInput=document.querySelector(\"input[name='wifi_pass']\");"
    "var note=document.getElementById('selectedNote');"
    "var selected=document.querySelectorAll('.wifi-item.selected');"
    "for(var i=0;i<selected.length;i++){selected[i].classList.remove('selected');}"
    "btn.classList.add('selected');"
    "ssidInput.value=ssid;"
    "if(secure){"
    "var entered=window.prompt('Wi-Fi password for ' + ssid, '');"
    "if(entered!==null){passInput.value=entered;}"
    "note.textContent='Selected ' + ssid + '. Verify or enter the password below.';"
    "}else{"
    "passInput.value='';"
    "note.textContent='Selected ' + ssid + '. This network does not require a password.';"
    "}"
    "passInput.focus();"
    "}"
    "</script>"
    "</div>"
    "</body>"
    "</html>";

static const char* kConfigPageTemplate =
    "<!doctype html>"
    "<html lang='en'>"
    "<head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Meeting Room Setup</title>"
    "<style>"
    ":root{color-scheme:dark;--bg:#0f1218;--panel:#171c25;--muted:#98a2b3;--line:#273142;--accent:#3fb983;--accent2:#"
    "5ba6ff;}"
    "*{box-sizing:border-box}"
    "body{margin:0;min-height:100vh;padding:24px;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe "
    "UI',sans-serif;color:#fff;background:radial-gradient(circle at top,#1a2330,#0b0d12 "
    "68%);display:flex;align-items:center;justify-content:center}"
    ".card{width:min(760px,100%%);background:rgba(23,28,37,.96);border:1px solid "
    "var(--line);border-radius:24px;padding:28px;"
    "box-shadow:0 24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:32px}"
    ".lead{margin:0 0 18px;color:var(--muted);font-size:17px}"
    ".ap{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:0 0 20px}"
    ".ap div{padding:14px 16px;border-radius:16px;background:#11161f;border:1px solid var(--line)}"
    ".ap small{display:block;margin-bottom:6px;color:var(--muted);text-transform:uppercase;letter-spacing:.06em;font-size:11px}"
    ".scan{display:grid;gap:12px;margin:0 0 22px}"
    ".scan h2{margin:0;font-size:20px}"
    ".scan p{margin:0;color:var(--muted);font-size:14px}"
    ".picker{padding:12px 14px;border-radius:14px;background:#10151d;border:1px solid "
    "var(--line);color:#dbe4f3;font-size:14px}"
    ".wifi-list{display:grid;gap:10px;max-height:220px;overflow:auto;padding-right:4px}"
    ".wifi-item{width:100%%;display:flex;align-items:center;justify-content:space-between;gap:14px;padding:14px "
    "16px;border-radius:16px;"
    "border:1px solid #2f3b4f;background:#10151d;color:#fff;text-align:left;cursor:pointer}"
    ".wifi-item:hover{border-color:#4a6287}"
    ".wifi-item.selected{border-color:var(--accent2);box-shadow:0 0 0 1px rgba(91,166,255,.32) inset}"
    ".wifi-name{display:block;font-weight:700}"
    ".wifi-meta{display:flex;align-items:center;justify-content:flex-end;gap:10px;flex-wrap:wrap;color:var(--muted);"
    "font-size:13px}"
    ".wifi-tag{display:inline-flex;align-items:center;padding:5px "
    "8px;border-radius:999px;background:rgba(63,185,131,.16);color:#d8ffe8;font-size:12px}"
    ".wifi-tag-open{background:rgba(91,166,255,.16);color:#dbe9ff}"
    ".scan-empty{padding:16px;border-radius:16px;background:#10151d;border:1px dashed #324055;color:var(--muted)}"
    "form{display:grid;gap:16px}"
    "label{display:grid;gap:8px;font-weight:600}"
    "input,textarea,.wifi-item{-webkit-appearance:none;appearance:none}"
    "input,textarea{width:100%%;border:1px solid #324055;border-radius:14px;background:#0f141c;color:#fff;padding:14px "
    "16px;font:inherit}"
    "input:focus,textarea:focus{outline:none;border-color:var(--accent2);box-shadow:0 0 0 1px rgba(91,166,255,.32)}"
    "textarea{min-height:120px;resize:vertical}"
    ".hint{margin-top:-6px;color:var(--muted);font-size:13px}"
    ".save-btn{border:0;border-radius:14px;padding:16px 18px;font:inherit;font-weight:700;cursor:pointer;"
    "background:linear-gradient(135deg,var(--accent),var(--accent2));color:#081018}"
    ".foot{margin-top:16px;color:var(--muted);font-size:13px}"
    "@media(max-width:640px){body{padding:14px;display:block}.card{padding:18px;border-radius:20px}h1{font-size:26px}"
    ".wifi-list{max-height:160px}.wifi-item{"
    "align-items:flex-start;flex-direction:column}.wifi-meta{justify-content:flex-start}}"
    "</style>"
    "</head>"
    "<body>"
    "<div class='card'>"
    "<h1>%s</h1>"
    "<p class='lead'>%s</p>"
    "%s"
    "%s"
    "<form method='POST' action='/save' accept-charset='utf-8' enctype='application/x-www-form-urlencoded'>"
    "%s"
    "%s"
    "<button class='save-btn' type='submit'>%s</button>"
    "</form>"
    "<p class='foot'>%s</p>"
    "<script>"
    "function selectNetwork(btn){"
    "var ssid=btn.getAttribute('data-ssid')||'';"
    "var secure=btn.getAttribute('data-secure')==='1';"
    "var ssidInput=document.querySelector(\"input[name='wifi_ssid']\");"
    "var passInput=document.querySelector(\"input[name='wifi_pass']\");"
    "if(!ssidInput||!passInput){return;}"
    "var note=document.getElementById('selectedNote');"
    "var selected=document.querySelectorAll('.wifi-item.selected');"
    "for(var i=0;i<selected.length;i++){selected[i].classList.remove('selected');}"
    "btn.classList.add('selected');"
    "ssidInput.value=ssid;"
    "if(!secure){"
    "passInput.value='';"
    "if(note){note.textContent='Selected ' + ssid + '. This network does not require a password.';}"
    "}else{"
    "if(note){note.textContent='Selected ' + ssid + '. Enter the Wi-Fi password below.';}"
    "}"
    "passInput.focus();"
    "}"
    "</script>"
    "</div>"
    "</body>"
    "</html>";

static const char* kSuccessPage =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;"
    "background:#0b0d12;color:#fff;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    ".card{max-width:520px;background:#171c25;border:1px solid #273142;border-radius:24px;padding:28px;box-shadow:0 "
    "24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:28px}"
    "p{margin:0;color:#98a2b3}"
    "</style></head><body><div class='card'><h1>Wi-Fi Settings Saved</h1>"
    "<p>The board is switching to the saved Wi-Fi network now. Reopen Device setup from the board after it comes online "
    "to add Info screen QR links.</p>"
    "</div></body></html>";

static const char* kRuntimeSuccessPageLocal =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;"
    "background:#0b0d12;color:#fff;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    ".card{max-width:520px;background:#171c25;border:1px solid #273142;border-radius:24px;padding:28px;box-shadow:0 "
    "24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:28px}"
    "p{margin:0;color:#98a2b3}"
    "</style></head><body><div class='card'><h1>Settings Saved</h1>"
    "<p>The board is applying the settings now. This page may stop responding for a moment while the board reconnects."
    "</p>"
    "</div></body></html>";

static const char* kRuntimeSuccessPageAp =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Saved</title>"
    "<style>"
    "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;padding:24px;"
    "background:#0b0d12;color:#fff;font:16px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif}"
    ".card{max-width:520px;background:#171c25;border:1px solid #273142;border-radius:24px;padding:28px;box-shadow:0 "
    "24px 60px rgba(0,0,0,.35)}"
    "h1{margin:0 0 10px;font-size:28px}"
    "p{margin:0;color:#98a2b3}"
    "</style></head><body><div class='card'><h1>Wi-Fi Settings Saved</h1>"
    "<p>The board is switching to the saved Wi-Fi network now. Once it reconnects, open Device setup again from the "
    "board to add Info screen QR links.</p>"
    "</div></body></html>";

static char* html_escape_dup(const char* src);
static bool form_get_value(const char* body, const char* key, char* out, size_t out_size);
static void ensure_scan_mutex(void);
static esp_err_t refresh_scan_results(void);
static void portal_scan_refresh_task(void* arg);
static void config_scan_results_refresh(void);
static BaseType_t create_portal_task(
    TaskFunction_t task_fn,
    const char* name,
    uint32_t stack_size,
    void* arg,
    UBaseType_t priority,
    TaskHandle_t* out_handle);
static char* build_scan_list_html(void);
static char* build_wifi_setup_page(void);
static char* build_config_page(void);
static esp_err_t handle_root_get(httpd_req_t* req);
static esp_err_t handle_favicon_get(httpd_req_t* req);
static esp_err_t handle_save_post(httpd_req_t* req);
static esp_err_t handle_not_found(httpd_req_t* req, httpd_err_code_t err);
static esp_err_t start_http_server(void);
static void stop_dns_server(void);
static esp_err_t start_dns_server(void);
static void dns_server_task(void* arg);
static void restart_task(void* arg);
static void portal_apply_saved_runtime_task(void* arg);
static void portal_restart_btn_cb(lv_event_t* e);
static esp_err_t restore_saved_wifi_connection(void);

static char* html_escape_dup(const char* src)
{
    if (! src) {
        src = "";
    }

    size_t len = strlen(src);
    size_t cap = len * 6 + 1;
    char* out  = malloc(cap);
    size_t pos = 0;

    if (! out) {
        return NULL;
    }

    for (size_t i = 0; i < len; ++i) {
        const char* rep = NULL;
        switch (src[i]) {
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        case '\'':
            rep = "&#39;";
            break;
        default:
            break;
        }

        if (rep) {
            size_t rep_len = strlen(rep);
            memcpy(out + pos, rep, rep_len);
            pos += rep_len;
        } else {
            out[pos++] = src[i];
        }
    }

    out[pos] = '\0';
    return out;
}

static void url_decode_inplace(char* value)
{
    char* src = value;
    char* dst = value;

    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
            continue;
        }

        if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = {src[1], src[2], '\0'};
            *dst++      = (char)strtol(hex, NULL, 16);
            src += 3;
            continue;
        }

        *dst++ = *src++;
    }

    *dst = '\0';
}

static bool form_get_value(const char* body, const char* key, char* out, size_t out_size)
{
    size_t key_len;
    const char* cursor;

    if (! body || ! key || ! out || out_size == 0) {
        return false;
    }

    key_len = strlen(key);
    cursor  = body;

    while (*cursor) {
        const char* pair_end  = strchr(cursor, '&');
        const char* value_sep = strchr(cursor, '=');
        size_t value_len;

        if (! pair_end) {
            pair_end = cursor + strlen(cursor);
        }

        if (! value_sep || value_sep > pair_end) {
            cursor = (*pair_end == '&') ? pair_end + 1 : pair_end;
            continue;
        }

        if ((size_t)(value_sep - cursor) == key_len && strncmp(cursor, key, key_len) == 0) {
            value_len = (size_t)(pair_end - value_sep - 1);
            if (value_len >= out_size) {
                value_len = out_size - 1;
            }
            memcpy(out, value_sep + 1, value_len);
            out[value_len] = '\0';
            url_decode_inplace(out);
            return true;
        }

        cursor = (*pair_end == '&') ? pair_end + 1 : pair_end;
    }

    out[0] = '\0';
    return false;
}

static void ensure_scan_mutex(void)
{
    if (! s_scan_mutex) {
        s_scan_mutex = xSemaphoreCreateMutex();
    }
}

static int compare_scan_records_by_rssi_desc(const void* lhs, const void* rhs)
{
    const wifi_ap_record_t* a = (const wifi_ap_record_t*)lhs;
    const wifi_ap_record_t* b = (const wifi_ap_record_t*)rhs;
    return (int)b->rssi - (int)a->rssi;
}

static bool auth_mode_requires_password(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
#ifdef WIFI_AUTH_OWE
    case WIFI_AUTH_OWE:
#endif
        return false;
    default:
        return true;
    }
}

static const char* auth_mode_label(wifi_auth_mode_t authmode)
{
    switch (authmode) {
    case WIFI_AUTH_OPEN:
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "WPA2-Ent";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
#ifdef WIFI_AUTH_WAPI_PSK
    case WIFI_AUTH_WAPI_PSK:
        return "WAPI";
#endif
#ifdef WIFI_AUTH_OWE
    case WIFI_AUTH_OWE:
        return "OWE";
#endif
    default:
        return "Secure";
    }
}

static bool scan_results_have_ssid(const portal_scan_result_t* results, size_t count, const char* ssid)
{
    for (size_t i = 0; i < count; ++i) {
        if (strcmp(results[i].ssid, ssid) == 0) {
            return true;
        }
    }
    return false;
}

static esp_err_t refresh_scan_results(void)
{
    wifi_scan_config_t scan_cfg                          = {0};
    wifi_ap_record_t* records                            = NULL;
    portal_scan_result_t staged[MAX_PORTAL_SCAN_RESULTS] = {0};
    uint16_t ap_num                                      = 0;
    size_t staged_count                                  = 0;
    esp_err_t err = ESP_FAIL;

    ensure_scan_mutex();
    if (! s_scan_mutex) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    scan_cfg.scan_type   = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.show_hidden = false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        err = esp_wifi_scan_start(&scan_cfg, true);
        if (err == ESP_OK) {
            break;
        }
        if ((err == ESP_ERR_WIFI_TIMEOUT || err == ESP_ERR_WIFI_STATE) && attempt < 2) {
            vTaskDelay(pdMS_TO_TICKS(250 + attempt * 250));
            continue;
        }
        break;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Portal Wi-Fi scan failed to start: %s", esp_err_to_name(err));
        goto cleanup;
    }

    err = esp_wifi_scan_get_ap_num(&ap_num);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Portal Wi-Fi scan count failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    if (ap_num == 0) {
        memset(s_scan_results, 0, sizeof(s_scan_results));
        s_scan_result_count = 0;
        ESP_LOGI(TAG, "Portal Wi-Fi scan: no visible networks found");
        err = ESP_OK;
        goto cleanup;
    }

    if (ap_num > MAX_PORTAL_SCAN_RECORDS) {
        ap_num = MAX_PORTAL_SCAN_RECORDS;
    }

    records = calloc(ap_num, sizeof(*records));
    if (! records) {
        err = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    err = esp_wifi_scan_get_ap_records(&ap_num, records);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Portal Wi-Fi scan fetch failed: %s", esp_err_to_name(err));
        goto cleanup;
    }

    qsort(records, ap_num, sizeof(*records), compare_scan_records_by_rssi_desc);

    for (uint16_t i = 0; i < ap_num && staged_count < MAX_PORTAL_SCAN_RESULTS; ++i) {
        const char* ssid = (const char*)records[i].ssid;

        if (! ssid || ssid[0] == '\0') {
            continue;
        }
        if (scan_results_have_ssid(staged, staged_count, ssid)) {
            continue;
        }

        strncpy(staged[staged_count].ssid, ssid, sizeof(staged[staged_count].ssid) - 1);
        staged[staged_count].ssid[sizeof(staged[staged_count].ssid) - 1] = '\0';
        staged[staged_count].rssi                                        = records[i].rssi;
        staged[staged_count].authmode                                    = records[i].authmode;
        staged_count++;
    }

    memset(s_scan_results, 0, sizeof(s_scan_results));
    memcpy(s_scan_results, staged, staged_count * sizeof(staged[0]));
    s_scan_result_count = staged_count;
    ESP_LOGI(TAG, "Portal Wi-Fi scan cached %u visible networks", (unsigned)staged_count);
    err = ESP_OK;

cleanup:
    free(records);
    xSemaphoreGive(s_scan_mutex);
    return err;
}

static void portal_scan_refresh_task(void* arg)
{
    LV_UNUSED(arg);

    for (int attempt = 0; attempt < 3; ++attempt) {
        if (!s_portal_running) {
            break;
        }

        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(1200));
        } else {
            vTaskDelay(pdMS_TO_TICKS(1500));
        }

        esp_err_t err = refresh_scan_results();
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Portal background Wi-Fi scan cached nearby networks");
            break;
        }

        ESP_LOGW(TAG,
                 "Portal background Wi-Fi scan attempt %d failed: %s",
                 attempt + 1,
                 esp_err_to_name(err));
    }

    s_scan_refresh_task_running = false;
    vTaskDelete(NULL);
}

static void config_scan_results_refresh(void)
{
    if (s_scan_refresh_task_running || !s_portal_running) {
        return;
    }

    s_scan_refresh_task_running = true;
    if (create_portal_task(portal_scan_refresh_task,
                           "portal_scan_refresh",
                           4096,
                           NULL,
                           tskIDLE_PRIORITY + 1,
                           NULL) != pdPASS) {
        s_scan_refresh_task_running = false;
        ESP_LOGW(TAG, "Unable to start background Wi-Fi scan for setup portal");
    }
}

static char* build_scan_list_html(void)
{
    char* html;
    size_t cap;
    size_t pos = 0;

    ensure_scan_mutex();
    if (! s_scan_mutex) {
        return NULL;
    }

    xSemaphoreTake(s_scan_mutex, portMAX_DELAY);

    if (s_scan_result_count == 0) {
        static const char* kEmpty = "<div class='scan-empty'>No visible Wi-Fi networks were cached for setup mode. You "
                                    "can still enter the SSID manually below.</div>";
        html                      = strdup(kEmpty);
        xSemaphoreGive(s_scan_mutex);
        return html;
    }

    cap  = 256 + s_scan_result_count * 512;
    html = malloc(cap);
    if (! html) {
        xSemaphoreGive(s_scan_mutex);
        return NULL;
    }
    html[0] = '\0';

    for (size_t i = 0; i < s_scan_result_count; ++i) {
        char* esc_ssid         = html_escape_dup(s_scan_results[i].ssid);
        const char* auth_label = auth_mode_label(s_scan_results[i].authmode);
        bool needs_password    = auth_mode_requires_password(s_scan_results[i].authmode);
        const char* tag_class  = needs_password ? "wifi-tag" : "wifi-tag wifi-tag-open";

        if (! esc_ssid) {
            free(html);
            xSemaphoreGive(s_scan_mutex);
            return NULL;
        }

        pos += (size_t)snprintf(
            html + pos, cap - pos,
            "<button class='wifi-item' type='button' data-ssid='%s' data-secure='%d' onclick='selectNetwork(this)'>"
            "<span class='wifi-name'>%s</span>"
            "<span class='wifi-meta'><span class='%s'>%s</span><span>%d dBm</span></span>"
            "</button>",
            esc_ssid, needs_password ? 1 : 0, esc_ssid, tag_class, auth_label, (int)s_scan_results[i].rssi);

        free(esc_ssid);

        if (pos >= cap) {
            break;
        }
    }

    xSemaphoreGive(s_scan_mutex);
    return html;
}

static char* build_wifi_setup_page(void)
{
    char wifi_ssid[33]   = {0};
    char wifi_pass[65]   = {0};
    char* esc_ssid       = NULL;
    char* esc_url        = NULL;
    char* esc_ap_ssid    = NULL;
    char* esc_ap_pass    = NULL;
    char* scan_html      = NULL;
    char* html           = NULL;
    size_t cap;

    if (wifi_manager_fetch_credentials(wifi_ssid, wifi_pass) != ESP_OK) {
        wifi_ssid[0] = '\0';
    }

    if (s_scan_result_count == 0) {
        esp_err_t scan_err = refresh_scan_results();
        if (scan_err != ESP_OK) {
            ESP_LOGW(TAG, "Config page lazy Wi-Fi scan failed: %s", esp_err_to_name(scan_err));
        }
    }

    ensure_ap_ssid();
    esc_ssid = html_escape_dup(wifi_ssid);
    esc_url = html_escape_dup(s_ap_url);
    esc_ap_ssid = html_escape_dup(s_ap_ssid);
    esc_ap_pass = html_escape_dup(AP_PASSWORD);
    scan_html = build_scan_list_html();

    if (!esc_ssid || !esc_url || !esc_ap_ssid || !esc_ap_pass || !scan_html) {
        goto cleanup;
    }

    cap = strlen(kWifiSetupPageTemplate) + strlen(esc_ap_ssid) + strlen(esc_ap_pass) + strlen(esc_url) +
          strlen(scan_html) + strlen(esc_ssid) + 128;
    html = malloc(cap);
    if (!html) {
        goto cleanup;
    }

    snprintf(html, cap, kWifiSetupPageTemplate, esc_ap_ssid, esc_ap_pass, esc_url, scan_html, esc_ssid);

cleanup:
    free(scan_html);
    free(esc_ap_pass);
    free(esc_ap_ssid);
    free(esc_url);
    free(esc_ssid);
    return html;
}

static char* build_config_page(void)
{
    info_links_store_data_t* info_links = NULL;
    const char* heading_text          = NULL;
    char* info_html                   = NULL;
    char* scan_html                   = NULL;
    char* form_fields_html            = NULL;
    char* extra_fields_html           = NULL;
    char* esc_company                 = NULL;
    char* esc_office                  = NULL;
    char* esc_panel_name              = NULL;
    char* esc_weather_api_key         = NULL;
    char* esc_url                     = NULL;
    char* esc_network                 = NULL;
    char* html                        = NULL;
    const char* lead_text             = NULL;
    const char* button_text           = NULL;
    const char* foot_text             = NULL;
    const bool local_mode             = (s_access_mode == CONFIG_PORTAL_ACCESS_LOCAL);
    char weather_api_key[64]          = {0};
    size_t cap;

    if (!local_mode) {
        return build_wifi_setup_page();
    }

    info_links = calloc(1, sizeof(*info_links));
    if (!info_links) {
        goto cleanup;
    }

    {
        const char* network_name = s_network_name[0] != '\0' ? s_network_name : "Current Wi-Fi";

        heading_text = "Device setup";
        lead_text = "Stay on the same Wi-Fi network as the board and update the weather API key plus Info screen QR links.";
        button_text = "Save settings";
        foot_text = "Wi-Fi settings stay unchanged on this page. The guest Wi-Fi QR on Info is generated "
                    "automatically from the saved Wi-Fi credentials.";

        info_links_store_get(info_links);
        weather_integration_get_api_key_copy(weather_api_key, sizeof(weather_api_key));
        esc_company = html_escape_dup(info_links->company_website);
        esc_office = html_escape_dup(info_links->office_map);
        esc_panel_name = html_escape_dup(device_name_store_get());
        esc_weather_api_key = html_escape_dup(weather_api_key);
        esc_url = html_escape_dup(s_ap_url);
        esc_network = html_escape_dup(network_name);
        scan_html = strdup("");
        form_fields_html = strdup("");

        if (!esc_company || !esc_office || !esc_panel_name || !esc_weather_api_key || !esc_url || !esc_network || !scan_html ||
            !form_fields_html) {
            goto cleanup;
        }

        cap = strlen(esc_url) + strlen(esc_network) + 128;
        info_html = malloc(cap);
        if (!info_html) {
            goto cleanup;
        }
        snprintf(info_html,
                 cap,
                 "<div class='ap'>"
                 "<div><small>Current Wi-Fi</small><strong>%s</strong></div>"
                 "<div><small>Open</small><strong>%s</strong></div>"
                 "</div>",
                 esc_network,
                 esc_url);

        cap = strlen(esc_company) + strlen(esc_office) + strlen(esc_panel_name) + strlen(esc_weather_api_key) + 2200;
        extra_fields_html = malloc(cap);
        if (!extra_fields_html) {
            goto cleanup;
        }
        snprintf(extra_fields_html,
                 cap,
                 "<div class='scan'>"
                 "<h2>Panel</h2>"
                 "<label>Panel name"
                 "<input type='text' name='panel_name' maxlength='32' placeholder='Meeting Room' value='%s'>"
                 "</label>"
                 "</div>"
                 "<div class='scan'>"
                 "<h2>Info Screen QR Links</h2>"
                 "<p>These links generate the QR codes on the Info screen. The guest Wi-Fi QR is created "
                 "automatically from the saved Wi-Fi SSID and password.</p>"
                 "<label>OpenWeather API key"
                 "<input type='text' name='weather_api_key' maxlength='63' placeholder='Enter API key' value='%s'>"
                 "</label>"
                 "<div class='hint'>Weather requests stay disabled until an API key is saved.</div>"
                 "<label>Company website"
                 "<input type='url' name='company_website_url' maxlength='512' placeholder='https://example.com' "
                 "value='%s'>"
                 "</label>"
                 "<label>Office map"
                 "<input type='url' name='office_map_url' maxlength='512' placeholder='https://example.com/map' "
                 "value='%s'>"
                 "</label>"
                 "<div class='hint'>Leave any Info QR field empty to hide that QR code on the Info screen.</div>"
                 "</div>",
                 esc_panel_name,
                 esc_weather_api_key,
                 esc_company,
                 esc_office);
    }

    cap = strlen(kConfigPageTemplate) + strlen(heading_text) + strlen(lead_text) + strlen(info_html) + strlen(scan_html) +
          strlen(form_fields_html) + strlen(extra_fields_html) + strlen(button_text) + strlen(foot_text) + 128;
    html = malloc(cap);
    if (! html) {
        goto cleanup;
    }

    snprintf(html,
             cap,
             kConfigPageTemplate,
             heading_text,
             lead_text,
             info_html,
             scan_html,
             form_fields_html,
             extra_fields_html,
             button_text,
             foot_text);

cleanup:
    free(extra_fields_html);
    free(form_fields_html);
    free(scan_html);
    free(info_html);
    free(esc_network);
    free(esc_url);
    free(esc_weather_api_key);
    free(esc_panel_name);
    free(esc_office);
    free(esc_company);
    free(info_links);
    return html;
}

static esp_err_t handle_root_get(httpd_req_t* req)
{
    char* html = build_config_page();

    if (! html) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to build page");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(html);
    return ESP_OK;
}

static esp_err_t handle_favicon_get(httpd_req_t* req)
{
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t handle_save_post(httpd_req_t* req)
{
    char* body                          = NULL;
    char wifi_ssid[33]                  = {0};
    char wifi_pass[65]                  = {0};
    char panel_name[DEVICE_NAME_STORE_MAX_LEN + 1]      = {0};
    char weather_api_key[64]            = {0};
    char company_website_url[INFO_LINKS_STORE_MAX_URL_LEN + 1] = {0};
    char office_map_url[INFO_LINKS_STORE_MAX_URL_LEN + 1]      = {0};
    info_links_store_data_t info_links                           = {0};
    bool has_wifi_ssid                  = false;
    bool has_panel_name                 = false;
    bool has_weather_api_key            = false;
    bool has_company_website_url        = false;
    bool has_office_map_url             = false;
    bool local_mode                     = (s_access_mode == CONFIG_PORTAL_ACCESS_LOCAL);
    int remaining;
    int offset    = 0;
    esp_err_t err = ESP_OK;

    if (req->content_len <= 0 || req->content_len > MAX_FORM_BODY) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid form body");
        return ESP_FAIL;
    }

    body = malloc((size_t)req->content_len + 1);
    if (! body) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }

    remaining = req->content_len;
    while (remaining > 0) {
        int received = httpd_req_recv(req, body + offset, remaining);
        if (received <= 0) {
            free(body);
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                httpd_resp_send_408(req);
                return ESP_ERR_TIMEOUT;
            }
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Receive failed");
            return ESP_FAIL;
        }
        offset += received;
        remaining -= received;
    }
    body[offset] = '\0';

    has_wifi_ssid = form_get_value(body, "wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    form_get_value(body, "wifi_pass", wifi_pass, sizeof(wifi_pass));
    has_panel_name = form_get_value(body, "panel_name", panel_name, sizeof(panel_name));
    has_weather_api_key = form_get_value(body, "weather_api_key", weather_api_key, sizeof(weather_api_key));
    has_company_website_url =
        form_get_value(body, "company_website_url", company_website_url, sizeof(company_website_url));
    has_office_map_url = form_get_value(body, "office_map_url", office_map_url, sizeof(office_map_url));
    free(body);

    if (!local_mode && (!has_wifi_ssid || wifi_ssid[0] == '\0')) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Wi-Fi SSID is required");
        return ESP_ERR_INVALID_ARG;
    }

    if (!local_mode) {
        err = wifi_manager_save_credentials(wifi_ssid, wifi_pass);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_manager_save_credentials failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Wi-Fi credentials");
            return err;
        }
    }

    if (local_mode) {
        if (has_panel_name) {
            err = device_name_store_set(panel_name);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "device_name_store_set failed: %s", esp_err_to_name(err));
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save panel name");
                return err;
            }
        }
        if (has_weather_api_key) {
            weather_integration_set_api_key(weather_api_key);
        }
        info_links_store_get(&info_links);
        if (has_company_website_url) {
            strlcpy(info_links.company_website, company_website_url, sizeof(info_links.company_website));
        }
        if (has_office_map_url) {
            strlcpy(info_links.office_map, office_map_url, sizeof(info_links.office_map));
        }
        err = info_links_store_set(&info_links);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "info_links_store_set failed: %s", esp_err_to_name(err));
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save Info screen links");
            return err;
        }
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (s_runtime_mode) {
        TaskHandle_t apply_task = NULL;
        if (!runtime_apply_in_progress()) {
            set_runtime_apply_in_progress(true);
            set_phase_status(CONFIG_PORTAL_PHASE_APPLYING, "Applying settings...");
            if (create_portal_task(portal_apply_saved_runtime_task, "cfg_apply", 4096, NULL, 5, &apply_task) != pdPASS) {
                set_runtime_apply_in_progress(false);
                set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Settings saved, but apply could not start");
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to apply settings");
                return ESP_ERR_NO_MEM;
            }
        }
        httpd_resp_sendstr(req, local_mode ? kRuntimeSuccessPageLocal : kRuntimeSuccessPageAp);
    } else {
        httpd_resp_sendstr(req, kSuccessPage);
        create_portal_task(restart_task, "cfg_restart", 2048, NULL, 5, NULL);
    }
    return ESP_OK;
}

static esp_err_t handle_not_found(httpd_req_t* req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t root      = {
        .uri      = "/",
        .method   = HTTP_GET,
        .handler  = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t generate_204 = {
        .uri      = "/generate_204",
        .method   = HTTP_GET,
        .handler  = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t fwlink = {
        .uri      = "/fwlink",
        .method   = HTTP_GET,
        .handler  = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t hotspot = {
        .uri      = "/hotspot-detect.html",
        .method   = HTTP_GET,
        .handler  = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t connectivity = {
        .uri      = "/connectivity-check.html",
        .method   = HTTP_GET,
        .handler  = handle_root_get,
        .user_ctx = NULL,
    };
    httpd_uri_t favicon = {
        .uri      = "/favicon.ico",
        .method   = HTTP_GET,
        .handler  = handle_favicon_get,
        .user_ctx = NULL,
    };
    httpd_uri_t save = {
        .uri      = "/save",
        .method   = HTTP_POST,
        .handler  = handle_save_post,
        .user_ctx = NULL,
    };

    config.server_port  = HTTP_PORT;
    config.stack_size   = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 4;
    config.backlog_conn = 2;
    config.lru_purge_enable = true;
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
    /* HTTP handlers save settings through NVS, which must not run on an external-memory task stack. */
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "httpd_start failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root), TAG, "register root failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &generate_204), TAG, "register generate_204 failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &fwlink), TAG, "register fwlink failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &hotspot), TAG, "register hotspot failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &connectivity), TAG, "register connectivity failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &favicon), TAG, "register favicon failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &save), TAG, "register save failed");
    httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, handle_not_found);

    return ESP_OK;
}

static BaseType_t create_portal_task(
    TaskFunction_t task_fn,
    const char* name,
    uint32_t stack_size,
    void* arg,
    UBaseType_t priority,
    TaskHandle_t* out_handle)
{
#if defined(CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY)
    return xTaskCreateWithCaps(
        task_fn,
        name,
        stack_size,
        arg,
        priority,
        out_handle,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return xTaskCreate(task_fn, name, stack_size, arg, priority, out_handle);
#endif
}

static void stop_dns_server(void)
{
    s_dns_running = false;

    if (s_dns_sock >= 0) {
        shutdown(s_dns_sock, SHUT_RDWR);
        close(s_dns_sock);
        s_dns_sock = -1;
    }
    if (s_dns_task) {
        for (int wait_ms = 0; s_dns_task && wait_ms < 300; wait_ms += 20) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static size_t dns_skip_question_name(const uint8_t* packet, size_t packet_len, size_t offset)
{
    while (offset < packet_len) {
        uint8_t len = packet[offset];
        if (len == 0) {
            return offset + 1;
        }
        if ((len & 0xC0) == 0xC0) {
            return offset + 2;
        }
        offset += (size_t)len + 1;
    }
    return packet_len;
}

static void dns_server_task(void* arg)
{
    uint8_t rx[512];
    uint8_t tx[512];
    struct sockaddr_in bind_addr = {0};
    uint32_t ap_ip_addr          = inet_addr(s_ap_url + strlen("http://"));

    (void)arg;

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_port        = htons(DNS_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(s_dns_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) != 0) {
        ESP_LOGE(TAG, "Failed to bind DNS socket");
        close(s_dns_sock);
        s_dns_sock = -1;
        s_dns_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS redirect server started on port %d", DNS_PORT);

    while (s_dns_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t rx_len       = recvfrom(s_dns_sock, rx, sizeof(rx), 0, (struct sockaddr*)&client_addr, &client_len);

        if (rx_len <= 0) {
            continue;
        }

        if (rx_len < 12) {
            continue;
        }

        size_t qname_end = dns_skip_question_name(rx, (size_t)rx_len, 12);
        if (qname_end + 4 > (size_t)rx_len) {
            continue;
        }

        uint16_t qtype      = (uint16_t)((rx[qname_end] << 8) | rx[qname_end + 1]);
        size_t question_len = qname_end + 4 - 12;
        size_t tx_len       = 0;
        bool answer_a       = (qtype == 1 || qtype == 255);

        memcpy(tx, rx, 2);
        tx[2]  = 0x81;
        tx[3]  = 0x80;
        tx[4]  = 0x00;
        tx[5]  = 0x01;
        tx[6]  = 0x00;
        tx[7]  = answer_a ? 0x01 : 0x00;
        tx[8]  = 0x00;
        tx[9]  = 0x00;
        tx[10] = 0x00;
        tx[11] = 0x00;
        tx_len = 12;

        memcpy(tx + tx_len, rx + 12, question_len);
        tx_len += question_len;

        if (answer_a) {
            tx[tx_len++] = 0xC0;
            tx[tx_len++] = 0x0C;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x01;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x01;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x3C;
            tx[tx_len++] = 0x00;
            tx[tx_len++] = 0x04;
            memcpy(tx + tx_len, &ap_ip_addr, 4);
            tx_len += 4;
        }

        sendto(s_dns_sock, tx, tx_len, 0, (struct sockaddr*)&client_addr, client_len);
    }

    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }

    s_dns_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_dns_server(void)
{
    if (s_dns_task) {
        return ESP_OK;
    }

    s_dns_running = true;
    if (create_portal_task(dns_server_task, "cfg_dns", 4096, NULL, 5, &s_dns_task) != pdPASS) {
        s_dns_running = false;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static esp_err_t restore_saved_wifi_connection(void)
{
    char ssid[33] = {0};
    char pass[65] = {0};
    esp_err_t fetch_err = wifi_manager_fetch_credentials(ssid, pass);

    if (fetch_err != ESP_OK || ssid[0] == '\0') {
        if (!wifi_manager_lock(UINT32_MAX)) {
            return ESP_ERR_TIMEOUT;
        }
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            wifi_manager_unlock();
            return err;
        }
        err = esp_wifi_disconnect();
        wifi_manager_unlock();
        if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
            return err;
        }
        return ESP_OK;
    }

    esp_err_t err = wifi_manager_init_sta(ssid, pass);
    if (err != ESP_OK) {
        return err;
    }
    return ESP_OK;
}

static void restart_task(void* arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(POST_SAVE_DELAY_MS));
    esp_restart();
}

static void portal_apply_saved_runtime_task(void* arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(POST_SAVE_DELAY_MS));
    config_portal_stop_runtime();
    set_runtime_apply_in_progress(false);
    vTaskDelete(NULL);
}

esp_err_t config_portal_request_boot_once(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_u8(handle, NVS_BOOT_ONCE_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

bool config_portal_consume_boot_request(void)
{
    nvs_handle_t handle;
    esp_err_t err;
    uint8_t flag = 0;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return false;
    }

    err = nvs_get_u8(handle, NVS_BOOT_ONCE_KEY, &flag);
    if (err != ESP_OK || flag == 0) {
        nvs_close(handle);
        return false;
    }

    err = nvs_erase_key(handle, NVS_BOOT_ONCE_KEY);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear one-shot portal flag: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

esp_err_t config_portal_reboot_into_setup_mode(void)
{
    esp_err_t err = config_portal_request_boot_once();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to request portal boot: %s", esp_err_to_name(err));
        return err;
    }

    esp_restart();
    return ESP_OK;
}

esp_err_t config_portal_start(void)
{
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t ap_cfg             = {0};
    esp_netif_ip_info_t ip_info      = {0};
    esp_err_t err;
    const char* ap_ssid = config_portal_get_ap_ssid();
    bool wifi_locked = false;

    if (s_portal_running) {
        return ESP_OK;
    }

    set_phase_status(CONFIG_PORTAL_PHASE_STARTING, "Preparing setup...");
    s_runtime_mode = false;
    s_access_mode = CONFIG_PORTAL_ACCESS_AP;
    s_network_name[0] = '\0';

    wifi_locked = wifi_manager_lock(UINT32_MAX);
    if (!wifi_locked) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Wi-Fi stack is busy");
        return ESP_ERR_TIMEOUT;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (! s_ap_netif) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not create setup Wi-Fi");
        wifi_manager_unlock();
        return ESP_FAIL;
    }

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (! s_sta_netif) {
        err = ESP_FAIL;
        goto fail;
    }

    err = esp_wifi_init(&wifi_init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        goto fail;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        goto fail;
    }

    strncpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char*)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len       = strlen(ap_ssid);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        goto fail;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save in portal mode: %s", esp_err_to_name(err));
    }

    err = refresh_scan_results();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without cached Wi-Fi list: %s", esp_err_to_name(err));
    }

    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_ap_url, sizeof(s_ap_url), "http://" IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(s_ap_url, AP_DEFAULT_URL, sizeof(s_ap_url) - 1);
        s_ap_url[sizeof(s_ap_url) - 1] = '\0';
    }

    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        goto fail;
    }

    err = start_dns_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "DNS server start failed: %s", esp_err_to_name(err));
        goto fail;
    }

    s_portal_running = true;
    s_access_mode = CONFIG_PORTAL_ACCESS_AP;
    set_phase_status(CONFIG_PORTAL_PHASE_RUNNING, "Setup is ready");
    wifi_manager_unlock();
    config_scan_results_refresh();
    ESP_LOGI(TAG, "Config portal started: SSID=%s URL=%s", ap_ssid, s_ap_url);
    return ESP_OK;

fail:
    if (wifi_locked) {
        wifi_manager_unlock();
    }
    ESP_LOGE(TAG, "Config portal start failed: %s", esp_err_to_name(err));
    set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Setup could not start");
    config_portal_stop();
    return err;
}

esp_err_t config_portal_start_runtime(void)
{
    wifi_config_t ap_cfg        = {0};
    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err               = ESP_OK;
    const char* ap_ssid         = config_portal_get_ap_ssid();
    bool wifi_locked            = false;
    bool auto_reconnect_prev    = wifi_manager_is_auto_reconnect_enabled();
    bool scan_cached            = false;

    if (s_portal_running && s_runtime_mode) {
        return ESP_OK;
    }

    set_phase_status(CONFIG_PORTAL_PHASE_STARTING, "Preparing setup...");
    s_access_mode = CONFIG_PORTAL_ACCESS_NONE;
    s_network_name[0] = '\0';
    s_runtime_auto_reconnect_prev = auto_reconnect_prev;

    wifi_locked = wifi_manager_lock(UINT32_MAX);
    if (!wifi_locked) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Wi-Fi stack is busy");
        return ESP_ERR_TIMEOUT;
    }

    wifi_manager_set_auto_reconnect_enabled(false);

    err = wifi_manager_init_sta(NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi_manager_init_sta(NULL) failed for runtime portal: %s", esp_err_to_name(err));
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Wi-Fi stack is not ready");
        goto fail;
    }

    s_sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (!s_sta_netif) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not access STA interface");
        err = ESP_FAIL;
        goto fail;
    }

    if (wifi_manager_is_connected() &&
        esp_netif_get_ip_info(s_sta_netif, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
        copy_connected_wifi_name(s_network_name, sizeof(s_network_name));
        snprintf(s_ap_url, sizeof(s_ap_url), "http://" IPSTR, IP2STR(&ip_info.ip));

        err = start_http_server();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Runtime local setup HTTP server start failed: %s", esp_err_to_name(err));
            set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not start the local setup page");
            goto fail;
        }

        s_portal_running = true;
        s_runtime_mode   = true;
        s_access_mode    = CONFIG_PORTAL_ACCESS_LOCAL;
        set_phase_status(CONFIG_PORTAL_PHASE_RUNNING, "Setup is ready");
        wifi_manager_unlock();
        wifi_locked = false;
        ESP_LOGI(TAG,
                 "Runtime config portal started on local Wi-Fi: SSID=%s URL=%s",
                 s_network_name[0] != '\0' ? s_network_name : "<unknown>",
                 s_ap_url);
        return ESP_OK;
    }

    wifi_manager_set_auto_reconnect_enabled(false);

    s_ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }
    if (!s_ap_netif) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not create AP interface");
        err = ESP_FAIL;
        goto fail;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not configure Wi-Fi storage");
        goto fail;
    }

    err = esp_wifi_disconnect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_CONNECT) {
        ESP_LOGW(TAG, "Runtime portal failed to disconnect STA before setup: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not prepare Wi-Fi scan");
        goto fail;
    }

    vTaskDelay(pdMS_TO_TICKS(300));

    err = refresh_scan_results();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Runtime portal pre-scan failed on first attempt: %s", esp_err_to_name(err));

        esp_err_t restart_err = esp_wifi_stop();
        if (restart_err != ESP_OK && restart_err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "Runtime portal Wi-Fi stop before rescan failed: %s", esp_err_to_name(restart_err));
        }

        vTaskDelay(pdMS_TO_TICKS(120));

        restart_err = esp_wifi_start();
        if (restart_err != ESP_OK) {
            ESP_LOGW(TAG, "Runtime portal Wi-Fi restart before rescan failed: %s", esp_err_to_name(restart_err));
        } else {
            esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
            if (ps_err != ESP_OK) {
                ESP_LOGW(TAG, "Runtime portal could not restore PS setting after Wi-Fi restart: %s",
                         esp_err_to_name(ps_err));
            }
            vTaskDelay(pdMS_TO_TICKS(250));
            err = refresh_scan_results();
        }
    }

    if (err == ESP_OK) {
        scan_cached = true;
    } else {
        ESP_LOGW(TAG, "Runtime portal continues without refreshed Wi-Fi list: %s", esp_err_to_name(err));
    }

    strncpy((char*)ap_cfg.ap.ssid, ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char*)ap_cfg.ap.password, AP_PASSWORD, sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len       = strlen(ap_ssid);
    ap_cfg.ap.channel        = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA_WPA2_PSK;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not switch Wi-Fi into setup mode");
        goto fail;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not configure setup access point");
        goto fail;
    }

    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable Wi-Fi power save in runtime portal mode: %s", esp_err_to_name(err));
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    if (esp_netif_get_ip_info(s_ap_netif, &ip_info) == ESP_OK) {
        snprintf(s_ap_url, sizeof(s_ap_url), "http://" IPSTR, IP2STR(&ip_info.ip));
    } else {
        strncpy(s_ap_url, AP_DEFAULT_URL, sizeof(s_ap_url) - 1);
        s_ap_url[sizeof(s_ap_url) - 1] = '\0';
    }

    err = start_http_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Runtime portal HTTP server start failed: %s", esp_err_to_name(err));
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not start the setup page");
        goto fail;
    }

    err = start_dns_server();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Runtime portal DNS start failed: %s", esp_err_to_name(err));
        if (s_http_server) {
            httpd_stop(s_http_server);
            s_http_server = NULL;
        }
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not start setup networking");
        goto fail;
    }

    s_portal_running = true;
    s_runtime_mode   = true;
    s_access_mode    = CONFIG_PORTAL_ACCESS_AP;
    set_phase_status(CONFIG_PORTAL_PHASE_RUNNING, "Setup is ready");
    wifi_manager_unlock();
    wifi_locked = false;
    ESP_LOGI(TAG, "Runtime config portal started in AP mode: SSID=%s URL=%s channel=%u scan_cached=%d",
             ap_ssid,
             s_ap_url,
             (unsigned)ap_cfg.ap.channel,
             scan_cached ? 1 : 0);
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "Runtime config portal start aborted: %s", esp_err_to_name(err));
    wifi_manager_set_auto_reconnect_enabled(auto_reconnect_prev);
    stop_dns_server();
    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }
    if (wifi_locked) {
        esp_err_t restore_err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (restore_err != ESP_OK && restore_err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "Failed to restore STA mode after runtime portal error: %s", esp_err_to_name(restore_err));
        }
        wifi_manager_unlock();
    }
    s_portal_running = false;
    s_runtime_mode   = false;
    s_access_mode    = CONFIG_PORTAL_ACCESS_NONE;
    s_network_name[0] = '\0';
    return err;
}

void config_portal_stop(void)
{
    bool wifi_locked = false;

    if (s_runtime_mode) {
        config_portal_stop_runtime();
        return;
    }

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    stop_dns_server();

    wifi_locked = wifi_manager_lock(UINT32_MAX);
    if (!wifi_locked) {
        ESP_LOGW(TAG, "Timeout obtaining Wi-Fi lock while stopping config portal");
    }

    if (s_portal_running || s_ap_netif) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }

    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }

    if (s_sta_netif) {
        esp_netif_destroy_default_wifi(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_portal_running = false;
    s_runtime_mode = false;
    s_access_mode = CONFIG_PORTAL_ACCESS_NONE;
    s_network_name[0] = '\0';
    s_scan_refresh_task_running = false;
    set_phase_status(CONFIG_PORTAL_PHASE_IDLE, "Setup inactive");

    if (wifi_locked) {
        wifi_manager_unlock();
    }
}

esp_err_t config_portal_stop_runtime(void)
{
    esp_err_t err = ESP_OK;
    bool wifi_locked = false;

    set_phase_status(CONFIG_PORTAL_PHASE_STOPPING, "Closing setup...");

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    stop_dns_server();

    wifi_locked = wifi_manager_lock(UINT32_MAX);
    if (!wifi_locked) {
        set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Wi-Fi stack is busy");
        return ESP_ERR_TIMEOUT;
    }

    if (s_portal_running || s_runtime_mode) {
        err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            wifi_manager_unlock();
            set_phase_status(CONFIG_PORTAL_PHASE_ERROR, "Could not leave setup mode");
            return err;
        }
    }

    if (s_access_mode == CONFIG_PORTAL_ACCESS_AP) {
        wifi_manager_set_auto_reconnect_enabled(s_runtime_auto_reconnect_prev);
    }
    wifi_manager_unlock();
    wifi_locked = false;

    s_portal_running = false;
    s_runtime_mode   = false;
    s_access_mode    = CONFIG_PORTAL_ACCESS_NONE;
    s_network_name[0] = '\0';
    s_scan_refresh_task_running = false;

    err = restore_saved_wifi_connection();
    if (err == ESP_OK) {
        set_phase_status(
            CONFIG_PORTAL_PHASE_IDLE,
            wifi_manager_is_connected() ? "Setup closed. Wi-Fi is connected." : "Setup closed. Reconnecting to Wi-Fi...");
    } else {
        ESP_LOGW(TAG, "Saved Wi-Fi reconnection did not start after closing setup: %s", esp_err_to_name(err));
        set_phase_status(CONFIG_PORTAL_PHASE_IDLE, "Setup closed. Wi-Fi reconnection needs a retry from the Wi-Fi page.");
    }

    return err;
}

bool config_portal_is_running(void) { return s_portal_running; }

bool config_portal_is_runtime_mode(void) { return s_runtime_mode; }

config_portal_access_mode_t config_portal_get_access_mode(void) { return s_access_mode; }

bool config_portal_requires_setup_wifi(void) { return s_access_mode == CONFIG_PORTAL_ACCESS_AP; }

config_portal_phase_t config_portal_get_phase(void)
{
    config_portal_phase_t phase = CONFIG_PORTAL_PHASE_IDLE;

    ensure_state_mutex();
    if (!s_state_mutex) {
        return s_phase;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    phase = s_phase;
    xSemaphoreGive(s_state_mutex);
    return phase;
}

void config_portal_get_status_copy(char* out, size_t out_size)
{
    if (!out || out_size == 0) {
        return;
    }

    ensure_state_mutex();
    if (!s_state_mutex) {
        out[0] = '\0';
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strncpy(out, s_status_text, out_size - 1);
    out[out_size - 1] = '\0';
    xSemaphoreGive(s_state_mutex);
}

const char* config_portal_get_ap_ssid(void)
{
    ensure_ap_ssid();
    return s_ap_ssid;
}

const char* config_portal_get_ap_password(void) { return AP_PASSWORD; }

const char* config_portal_get_ap_url(void) { return s_ap_url; }

const char* config_portal_get_network_name(void)
{
    if (s_access_mode == CONFIG_PORTAL_ACCESS_LOCAL && s_network_name[0] != '\0') {
        return s_network_name;
    }

    if (s_access_mode == CONFIG_PORTAL_ACCESS_AP) {
        return config_portal_get_ap_ssid();
    }

    return "";
}

static void portal_restart_btn_cb(lv_event_t* e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) {
        return;
    }

    config_portal_stop();
    esp_restart();
}

static lv_obj_t* portal_create_step_tile(lv_obj_t* parent, lv_coord_t width, const char* step_text,
                                         const char* title_text, const char* description_text, const char* detail_text,
                                         const char* qr_payload, lv_color_t accent)
{
    lv_coord_t content_width = width - 32;
    lv_coord_t info_width    = content_width;
    lv_obj_t* tile           = lv_obj_create(parent);
    lv_obj_t* body;
    lv_obj_t* badge;
    lv_obj_t* title;
    lv_obj_t* info;
    lv_obj_t* description;
    lv_obj_t* detail;

    lv_obj_set_size(tile, width, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(tile, 252, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(tile, 22, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0x10151d), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(tile, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(tile, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_opa(tile, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(tile, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(tile, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(tile, LV_OPA_30, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(tile, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(tile, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(tile, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(tile, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(tile, 4, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

    badge = lv_label_create(tile);
    lv_label_set_text(badge, step_text);
    lv_obj_set_style_text_font(badge, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(badge, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(badge, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(badge, LV_OPA_20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(badge, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(badge, 12, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(badge, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(badge, 5, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, LV_PART_MAIN | LV_STATE_DEFAULT);

    title = lv_label_create(tile);
    lv_label_set_text(title, title_text);
    lv_obj_set_width(title, content_width);
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title, &ui_font_Roboto20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    body = lv_obj_create(tile);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, content_width, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(body, 162, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(body, 12, LV_PART_MAIN | LV_STATE_DEFAULT);

#if LV_USE_QRCODE
    if (qr_payload && qr_payload[0] != '\0') {
        lv_obj_t* qr_frame = lv_obj_create(body);
        lv_obj_t* qr;

        lv_obj_set_size(qr_frame, 148, 148);
        lv_obj_set_style_radius(qr_frame, 16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(qr_frame, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(qr_frame, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(qr_frame, accent, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(qr_frame, LV_OPA_30, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_all(qr_frame, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_clear_flag(qr_frame, LV_OBJ_FLAG_SCROLLABLE);

        qr = lv_qrcode_create(qr_frame, 128, lv_color_hex(0x0B1220), lv_color_hex(0xFFFFFF));
        lv_qrcode_update(qr, qr_payload, strlen(qr_payload));
        lv_obj_center(qr);
        info_width = content_width - 160;
    }
#endif

    info = lv_obj_create(body);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, info_width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(info, 4, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (description_text && description_text[0] != '\0') {
        description = lv_label_create(info);
        lv_label_set_text(description, description_text);
        lv_obj_set_width(description, info_width);
        lv_label_set_long_mode(description, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(description, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(description, lv_color_hex(0xA0ACC0), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_align(description, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    detail = lv_label_create(info);
    lv_label_set_text(detail, (detail_text && detail_text[0] != '\0') ? detail_text : "");
    lv_obj_set_width(detail, info_width);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(detail, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(detail, lv_color_hex(0xE2E8F0), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_LEFT, LV_PART_MAIN | LV_STATE_DEFAULT);

    return tile;
}

void config_portal_show_screen(bool portal_ready, const char* detail)
{
    lv_obj_t* card;
    lv_obj_t* title;
    lv_obj_t* subtitle;
    lv_obj_t* status;
    lv_obj_t* row;
    lv_obj_t* note;
    lv_obj_t* button;
    lv_obj_t* button_label;
    char wifi_qr_payload[128];
    char url_qr_payload[64];
    char step1_detail[160];
    char step2_detail[96];
    const char* ap_url  = config_portal_get_ap_url();
    const char* ap_host = ap_url;

    if (strncmp(ap_host, "http://", 7) == 0) {
        ap_host += 7;
    }

    snprintf(wifi_qr_payload, sizeof(wifi_qr_payload), "WIFI:T:WPA;S:%s;P:%s;;", config_portal_get_ap_ssid(),
             AP_PASSWORD);
    snprintf(url_qr_payload, sizeof(url_qr_payload), "%s", ap_url);
    snprintf(step1_detail, sizeof(step1_detail), "Wi-Fi: %s\nPassword: %s", config_portal_get_ap_ssid(),
             config_portal_get_ap_password());
    snprintf(step2_detail, sizeof(step2_detail), "Open: %s", ap_host);

    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen = NULL;
    }

    s_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x0d1016), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    card = lv_obj_create(s_screen);
    lv_obj_set_size(card, 760, 460);
    lv_obj_center(card);
    lv_obj_set_style_radius(card, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x171c25), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_color(card, lv_color_hex(0x283345), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_width(card, 32, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(card, LV_OPA_40, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(card, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(card, 24, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_top(card, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(card, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    title = lv_label_create(card);
    lv_label_set_text(title, portal_ready ? "Device setup" : "Device setup unavailable");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(title, &ui_font_Roboto40, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (! portal_ready) {
        subtitle = lv_label_create(card);
        lv_label_set_text(subtitle, "The board entered setup boot mode, but the setup page could not start correctly.");
        lv_obj_set_width(subtitle, 708);
        lv_label_set_long_mode(subtitle, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(subtitle, lv_color_hex(0xA0ACC0), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(subtitle, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 708, portal_ready ? LV_SIZE_CONTENT : 168);
    lv_obj_set_flex_flow(row, portal_ready ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(row, 8, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (portal_ready) {
        portal_create_step_tile(row, 345, "STEP 1", "Connect to the setup Wi-Fi", NULL,
                                step1_detail, wifi_qr_payload, lv_color_hex(0x5BA6FF));

        portal_create_step_tile(row, 345, "STEP 2", "Open the setup page", NULL, step2_detail, url_qr_payload,
                                lv_color_hex(0x6EE7B7));
    } else {
        lv_obj_t* error_tile = lv_obj_create(row);
        lv_obj_t* error_title;
        lv_obj_t* error_text;

        lv_obj_set_size(error_tile, 664, 168);
        lv_obj_set_style_radius(error_tile, 20, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(error_tile, lv_color_hex(0x10151d), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(error_tile, 1, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(error_tile, lv_color_hex(0x8B2E3B), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_left(error_tile, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_right(error_tile, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_top(error_tile, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_bottom(error_tile, 18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_pad_row(error_tile, 10, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_flex_flow(error_tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(error_tile, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_clear_flag(error_tile, LV_OBJ_FLAG_SCROLLABLE);

        error_title = lv_label_create(error_tile);
        lv_label_set_text(error_title, "Setup page could not start");
        lv_obj_set_style_text_font(error_title, &ui_font_Roboto26, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(error_title, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);

        error_text = lv_label_create(error_tile);
        lv_label_set_text(
            error_text,
            detail
                ? detail
                : "The board is in one-shot setup boot mode, but the access point and local page are not available.");
        lv_obj_set_width(error_text, 620);
        lv_label_set_long_mode(error_text, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(error_text, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_color(error_text, lv_color_hex(0xA0ACC0), LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    status = lv_label_create(card);
    lv_label_set_text(
        status, portal_ready ? (detail ? detail : "Open the page and save the settings to reboot into normal mode.")
                             : "Setup page is not available. Restart to return to normal mode.");
    lv_obj_set_width(status, 708);
    lv_label_set_long_mode(status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(status, portal_ready ? lv_color_hex(0x6EE7B7) : lv_color_hex(0xFCA5A5),
                                LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(status, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);

    if (!portal_ready) {
        note = lv_label_create(card);
        lv_label_set_text(note, "One-shot rule: after any reboot the firmware goes back to the normal flow.");
        lv_obj_set_width(note, 708);
        lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(note, lv_color_hex(0xA0ACC0), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_text_font(note, &ui_font_Roboto16, LV_PART_MAIN | LV_STATE_DEFAULT);
    }

    button = lv_btn_create(card);
    lv_obj_set_size(button, 228, 48);
    lv_obj_set_style_radius(button, 14, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x3B82F6), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(button, portal_restart_btn_cb, LV_EVENT_CLICKED, NULL);

    button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Restart To Normal Mode");
    lv_obj_set_style_text_font(button_label, &ui_font_Roboto18, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(button_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_center(button_label);

    lv_scr_load(s_screen);
}
