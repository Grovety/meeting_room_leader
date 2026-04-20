#include "aux_udp_link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>

#include "aux_udp_protocol.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

extern "C" {
#include "device_name_store.h"
#include "nvs.h"
#include "ui.h"
#include "wifi_manager.h"
}

static const char *TAG = "aux_udp_tx";

#define AUX_UDP_TX_PERIOD_MS        1000
#define AUX_UDP_HEARTBEAT_MS        2000
#define AUX_UDP_BEACON_MS           2000
#define AUX_UDP_PAIR_NAMESPACE      "aux_udp_pair"
#define AUX_UDP_PAIR_KEY_AUX_ID     "aux_id"
#define AUX_UDP_PAIR_KEY_TOKEN      "token"
#define AUX_UDP_PAIR_WINDOW_DEFAULT_SEC 60
#ifndef AUX_UDP_RESET_PAIR_ON_BOOT
#define AUX_UDP_RESET_PAIR_ON_BOOT 0
#endif

typedef struct {
    char room_name[AUX_UDP_ROOM_NAME_LEN];
    char clock_text[AUX_UDP_CLOCK_TEXT_LEN];
    bool occupied;
    int32_t remaining_sec;
    bool has_next_booking;
    char next_time_range[AUX_UDP_TIME_RANGE_LEN];
    char next_date_text[AUX_UDP_NEXT_DATE_LEN];
    char next_title[AUX_UDP_NEXT_TITLE_LEN];
    char next_booked_by[AUX_UDP_NEXT_BOOKED_BY_LEN];
} aux_udp_room_state_t;

typedef struct {
    bool paired;
    char aux_id[AUX_UDP_AUX_ID_LEN];
    char pair_token[AUX_UDP_PAIR_TOKEN_LEN];
} aux_udp_pair_state_t;

static TaskHandle_t s_aux_udp_task = NULL;
static char s_main_id[AUX_UDP_MAIN_ID_LEN] = {0};
static aux_udp_pair_state_t s_pair_state = {0};
static bool s_pair_state_loaded = false;
static struct sockaddr_in s_paired_aux_addr = {0};
static bool s_have_paired_aux_addr = false;
static int64_t s_pair_mode_deadline_us = 0;

static void copy_string_field(const char *src, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    if (!src || !src[0]) {
        return;
    }

    snprintf(dst, dst_size, "%s", src);
    dst[dst_size - 1] = '\0';
}

static void build_main_id(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};

    if (!out || out_size == 0) {
        return;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "main-%02X%02X%02X", mac[3], mac[4], mac[5]);
    out[out_size - 1] = '\0';
}

static void get_room_name(char *out, size_t out_size)
{
    const char *name = device_name_store_get();

    if (!out || out_size == 0) {
        return;
    }

    if (name && name[0] != '\0') {
        copy_string_field(name, out, out_size);
    } else {
        copy_string_field("Meeting Room", out, out_size);
    }
}

static void generate_pair_token(char *out, size_t out_size)
{
    uint8_t random_bytes[16] = {0};
    size_t pos = 0;

    if (!out || out_size == 0) {
        return;
    }

    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes) && pos + 2 < out_size; ++i) {
        pos += (size_t)snprintf(out + pos, out_size - pos, "%02x", random_bytes[i]);
    }
    out[out_size - 1] = '\0';
}

static bool is_pair_mode_enabled(void)
{
    if (!s_pair_state.paired) {
        return true;
    }

    if (s_pair_mode_deadline_us <= 0) {
        return false;
    }

    return esp_timer_get_time() < s_pair_mode_deadline_us;
}

static void set_pair_mode_window(uint32_t duration_sec)
{
    if (duration_sec == 0) {
        s_pair_mode_deadline_us = 0;
        return;
    }

    s_pair_mode_deadline_us = esp_timer_get_time() + ((int64_t)duration_sec * 1000000LL);
}

static void load_pair_state(void)
{
    nvs_handle_t handle;
    size_t aux_id_len = sizeof(s_pair_state.aux_id);
    size_t token_len = sizeof(s_pair_state.pair_token);

    if (s_pair_state_loaded) {
        return;
    }

    memset(&s_pair_state, 0, sizeof(s_pair_state));
    s_pair_state_loaded = true;

    if (nvs_open(AUX_UDP_PAIR_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return;
    }

    if (nvs_get_str(handle, AUX_UDP_PAIR_KEY_AUX_ID, s_pair_state.aux_id, &aux_id_len) == ESP_OK &&
        nvs_get_str(handle, AUX_UDP_PAIR_KEY_TOKEN, s_pair_state.pair_token, &token_len) == ESP_OK &&
        s_pair_state.aux_id[0] != '\0' &&
        s_pair_state.pair_token[0] != '\0') {
        s_pair_state.paired = true;
    }

    nvs_close(handle);
}

static esp_err_t persist_pair_state(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(AUX_UDP_PAIR_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    if (s_pair_state.paired) {
        err = nvs_set_str(handle, AUX_UDP_PAIR_KEY_AUX_ID, s_pair_state.aux_id);
        if (err == ESP_OK) {
            err = nvs_set_str(handle, AUX_UDP_PAIR_KEY_TOKEN, s_pair_state.pair_token);
        }
    } else {
        nvs_erase_key(handle, AUX_UDP_PAIR_KEY_AUX_ID);
        nvs_erase_key(handle, AUX_UDP_PAIR_KEY_TOKEN);
    }

    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void format_time_range_text(time_t start, time_t end, char *out, size_t out_size)
{
    struct tm start_tm = {};
    struct tm end_tm = {};
    char start_text[8] = {};
    char end_text[8] = {};

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (start <= 0) {
        return;
    }

    localtime_r(&start, &start_tm);
    strftime(start_text, sizeof(start_text), "%H:%M", &start_tm);

    if (end > 0) {
        localtime_r(&end, &end_tm);
        strftime(end_text, sizeof(end_text), "%H:%M", &end_tm);
        snprintf(out, out_size, "%s-%s", start_text, end_text);
    } else {
        snprintf(out, out_size, "%s", start_text);
    }

    out[out_size - 1] = '\0';
}

static void format_next_booking_date_text(time_t now, time_t next_start, char *out, size_t out_size)
{
    struct tm now_tm = {};
    struct tm next_tm = {};

    if (!out || out_size == 0) {
        return;
    }

    out[0] = '\0';
    if (now <= 0 || next_start <= 0) {
        return;
    }

    localtime_r(&now, &now_tm);
    localtime_r(&next_start, &next_tm);

    if (now_tm.tm_year == next_tm.tm_year && now_tm.tm_yday == next_tm.tm_yday) {
        return;
    }

    strftime(out, out_size, "%a, %d %b", &next_tm);
    out[out_size - 1] = '\0';
}

static void capture_room_state(aux_udp_room_state_t *state)
{
    time_t now = time(NULL);
    struct tm now_tm = {};
    int manual_remaining_sec = ui_get_booking_remaining_sec_snapshot();

    if (!state) {
        return;
    }

    memset(state, 0, sizeof(*state));
    get_room_name(state->room_name, sizeof(state->room_name));
    copy_string_field("--:--", state->clock_text, sizeof(state->clock_text));
    copy_string_field("--", state->next_time_range, sizeof(state->next_time_range));
    copy_string_field("No upcoming bookings", state->next_title, sizeof(state->next_title));

    localtime_r(&now, &now_tm);
    strftime(state->clock_text, sizeof(state->clock_text), "%H:%M", &now_tm);

    if (manual_remaining_sec > 0) {
        state->occupied = true;
        state->remaining_sec = manual_remaining_sec;
    }
}

static int open_udp_socket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    int reuse_addr = 1;
    int enable_broadcast = 1;
    struct sockaddr_in bind_addr = {0};

    if (sock < 0) {
        return -1;
    }

    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse_addr, sizeof(reuse_addr)) < 0) {
        close(sock);
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable_broadcast, sizeof(enable_broadcast)) < 0) {
        close(sock);
        return -1;
    }

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port = htons(AUX_UDP_PORT);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static void build_broadcast_target(struct sockaddr_in *out_addr)
{
    esp_netif_t *sta_netif = NULL;
    esp_netif_ip_info_t ip_info = {};

    if (!out_addr) {
        return;
    }

    memset(out_addr, 0, sizeof(*out_addr));
    out_addr->sin_family = AF_INET;
    out_addr->sin_port = htons(AUX_UDP_PORT);
    out_addr->sin_addr.s_addr = htonl(INADDR_BROADCAST);

    sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!sta_netif) {
        return;
    }

    if (esp_netif_get_ip_info(sta_netif, &ip_info) != ESP_OK || ip_info.netmask.addr == 0) {
        return;
    }

    out_addr->sin_addr.s_addr = (ip_info.ip.addr & ip_info.netmask.addr) | (~ip_info.netmask.addr);
}

static void fill_common_packet_fields(aux_udp_packet_v1_t *packet,
                                      uint8_t msg_type,
                                      uint32_t seq,
                                      uint32_t state_rev,
                                      const char *room_name)
{
    if (!packet) {
        return;
    }

    memset(packet, 0, sizeof(*packet));
    packet->magic = AUX_UDP_MAGIC;
    packet->version = AUX_UDP_VERSION;
    packet->packet_size = (uint16_t)sizeof(*packet);
    packet->msg_type = msg_type;
    packet->seq = seq;
    packet->state_rev = state_rev;
    packet->sent_epoch_sec = (int64_t)time(NULL);

    copy_string_field(s_main_id, packet->main_id, sizeof(packet->main_id));
    if (room_name) {
        copy_string_field(room_name, packet->room_name, sizeof(packet->room_name));
    }

    if (s_pair_state.paired) {
        packet->flags |= AUX_UDP_FLAG_MAIN_PAIRED;
        copy_string_field(s_pair_state.aux_id, packet->aux_id, sizeof(packet->aux_id));
        copy_string_field(s_pair_state.pair_token, packet->pair_token, sizeof(packet->pair_token));
    }
    if (is_pair_mode_enabled()) {
        packet->flags |= AUX_UDP_FLAG_MAIN_PAIR_MODE;
    }
}

static void finalize_packet_crc(aux_udp_packet_v1_t *packet)
{
    if (!packet) {
        return;
    }

    packet->crc32 = 0;
    packet->crc32 = esp_rom_crc32_le(0, (const uint8_t *)packet, sizeof(*packet));
}

static bool validate_packet(const aux_udp_packet_v1_t *packet, size_t len)
{
    aux_udp_packet_v1_t copy;
    uint32_t expected_crc = 0;

    if (!packet || len != sizeof(aux_udp_packet_v1_t)) {
        return false;
    }

    copy = *packet;
    if (copy.magic != AUX_UDP_MAGIC ||
        copy.version != AUX_UDP_VERSION ||
        copy.packet_size != sizeof(aux_udp_packet_v1_t)) {
        return false;
    }

    expected_crc = copy.crc32;
    copy.crc32 = 0;
    return esp_rom_crc32_le(0, (const uint8_t *)&copy, sizeof(copy)) == expected_crc;
}

static esp_err_t send_packet(int sock, const aux_udp_packet_v1_t *packet, const struct sockaddr_in *target)
{
    struct sockaddr_in broadcast_target = {0};
    const struct sockaddr_in *dst = target;
    ssize_t sent = 0;

    if (sock < 0 || !packet) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!dst) {
        build_broadcast_target(&broadcast_target);
        dst = &broadcast_target;
    }

    sent = sendto(sock, packet, sizeof(*packet), 0, (const struct sockaddr *)dst, sizeof(*dst));
    if (sent != (ssize_t)sizeof(*packet)) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool handle_pair_request(int sock,
                                const aux_udp_packet_v1_t *request,
                                const struct sockaddr_in *source_addr,
                                const char *current_room_name,
                                uint32_t *seq)
{
    aux_udp_packet_v1_t response = {0};
    bool changed = false;
    bool request_main_match = false;
    bool request_room_match = false;
    bool same_aux = false;
    bool accepted = false;

    if (!request || !source_addr || !seq || !request->aux_id[0]) {
        return false;
    }

    request_main_match = (request->main_id[0] == '\0') ||
                         (strcmp(request->main_id, s_main_id) == 0);
    request_room_match = (request->room_name[0] == '\0') ||
                         (current_room_name && strcmp(request->room_name, current_room_name) == 0);

    if (!request_main_match || !request_room_match) {
        return false;
    }

    same_aux = s_pair_state.paired && (strcmp(s_pair_state.aux_id, request->aux_id) == 0);
    accepted = same_aux || !s_pair_state.paired || is_pair_mode_enabled();

    if (accepted) {
        if (!same_aux) {
            memset(&s_pair_state, 0, sizeof(s_pair_state));
            s_pair_state.paired = true;
            copy_string_field(request->aux_id, s_pair_state.aux_id, sizeof(s_pair_state.aux_id));
            generate_pair_token(s_pair_state.pair_token, sizeof(s_pair_state.pair_token));
            if (persist_pair_state() == ESP_OK) {
                ESP_LOGI(TAG, "Paired with aux '%s'", s_pair_state.aux_id);
            } else {
                ESP_LOGW(TAG, "Failed to persist pair state");
            }
            changed = true;
            set_pair_mode_window(0);
        }

        s_paired_aux_addr = *source_addr;
        s_paired_aux_addr.sin_port = htons(AUX_UDP_PORT);
        s_have_paired_aux_addr = true;
    }

    fill_common_packet_fields(&response,
                              accepted ? AUX_UDP_MSG_PAIR_ACK : AUX_UDP_MSG_PAIR_NACK,
                              (*seq)++,
                              0,
                              current_room_name);
    copy_string_field(request->aux_id, response.aux_id, sizeof(response.aux_id));
    finalize_packet_crc(&response);
    (void)send_packet(sock, &response, source_addr);

    return changed;
}

static bool process_incoming_messages(int sock, const char *current_room_name, uint32_t *seq)
{
    bool changed = false;

    while (true) {
        aux_udp_packet_v1_t packet = {0};
        struct sockaddr_in source_addr = {0};
        socklen_t source_addr_len = sizeof(source_addr);
        int len = recvfrom(sock,
                           &packet,
                           sizeof(packet),
                           MSG_DONTWAIT,
                           (struct sockaddr *)&source_addr,
                           &source_addr_len);

        if (len < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break;
            }
            break;
        }

        if (!validate_packet(&packet, (size_t)len)) {
            continue;
        }

        if (packet.msg_type == AUX_UDP_MSG_PAIR_REQ) {
            if (handle_pair_request(sock, &packet, &source_addr, current_room_name, seq)) {
                changed = true;
            }
        }
    }

    return changed;
}

static esp_err_t send_beacon_packet(int sock, const char *room_name, uint32_t state_rev, uint32_t *seq)
{
    aux_udp_packet_v1_t packet = {0};

    if (sock < 0 || !seq) {
        return ESP_ERR_INVALID_ARG;
    }

    fill_common_packet_fields(&packet, AUX_UDP_MSG_BEACON, (*seq)++, state_rev, room_name);
    finalize_packet_crc(&packet);
    return send_packet(sock, &packet, NULL);
}

static esp_err_t send_state_packet(int sock,
                                   const aux_udp_room_state_t *state,
                                   uint32_t state_rev,
                                   uint32_t *seq)
{
    aux_udp_packet_v1_t packet = {0};

    if (sock < 0 || !state || !seq) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_pair_state.paired || !s_have_paired_aux_addr) {
        return ESP_ERR_INVALID_STATE;
    }

    fill_common_packet_fields(&packet, AUX_UDP_MSG_STATE, (*seq)++, state_rev, state->room_name);

    packet.occupied = state->occupied ? 1 : 0;
    packet.has_next_booking = state->has_next_booking ? 1 : 0;
    packet.remaining_sec = state->remaining_sec;

    copy_string_field(state->clock_text, packet.clock_text, sizeof(packet.clock_text));
    copy_string_field(state->next_time_range, packet.next_time_range, sizeof(packet.next_time_range));
    copy_string_field(state->next_date_text, packet.next_date_text, sizeof(packet.next_date_text));
    copy_string_field(state->next_title, packet.next_title, sizeof(packet.next_title));
    copy_string_field(state->next_booked_by, packet.next_booked_by, sizeof(packet.next_booked_by));

    finalize_packet_crc(&packet);
    return send_packet(sock, &packet, &s_paired_aux_addr);
}

static void aux_udp_task(void *arg)
{
    aux_udp_room_state_t prev_state = {};
    bool has_prev_state = false;
    int sock = -1;
    uint32_t seq = 1;
    uint32_t state_rev = 0;
    int64_t last_sent_us = 0;
    int64_t last_beacon_us = 0;

    (void)arg;

    build_main_id(s_main_id, sizeof(s_main_id));
    load_pair_state();
    if (AUX_UDP_RESET_PAIR_ON_BOOT) {
        memset(&s_pair_state, 0, sizeof(s_pair_state));
        s_have_paired_aux_addr = false;
        (void)persist_pair_state();
        ESP_LOGW(TAG, "Pair state cleared on boot by AUX_UDP_RESET_PAIR_ON_BOOT");
    }
    if (s_pair_state.paired) {
        ESP_LOGI(TAG, "UDP link started: main_id=%s paired=yes aux=%s", s_main_id, s_pair_state.aux_id);
    } else {
        set_pair_mode_window(AUX_UDP_PAIR_WINDOW_DEFAULT_SEC);
        ESP_LOGI(TAG, "UDP link started: main_id=%s paired=no", s_main_id);
    }

    while (true) {
        if (!wifi_manager_is_connected()) {
            if (sock >= 0) {
                close(sock);
                sock = -1;
            }
            has_prev_state = false;
            s_have_paired_aux_addr = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (sock < 0) {
            sock = open_udp_socket();
            if (sock < 0) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
        }

        aux_udp_room_state_t state = {};
        capture_room_state(&state);

        const bool changed_pair = process_incoming_messages(sock, state.room_name, &seq);
        const bool changed_state = !has_prev_state || memcmp(&state, &prev_state, sizeof(state)) != 0;
        const int64_t now_us = esp_timer_get_time();
        const bool heartbeat_due = (last_sent_us == 0) || (now_us - last_sent_us >= (AUX_UDP_HEARTBEAT_MS * 1000LL));
        const bool beacon_due = (last_beacon_us == 0) || (now_us - last_beacon_us >= (AUX_UDP_BEACON_MS * 1000LL));

        if (beacon_due) {
            (void)send_beacon_packet(sock, state.room_name, state_rev, &seq);
            last_beacon_us = now_us;
        }

        if (changed_state || changed_pair || heartbeat_due) {
            if (changed_state || changed_pair) {
                ++state_rev;
            }

            if (s_pair_state.paired && s_have_paired_aux_addr) {
                if (send_state_packet(sock, &state, state_rev, &seq) == ESP_OK) {
                    prev_state = state;
                    has_prev_state = true;
                    last_sent_us = now_us;
                } else {
                    close(sock);
                    sock = -1;
                    vTaskDelay(pdMS_TO_TICKS(1000));
                    continue;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(AUX_UDP_TX_PERIOD_MS));
    }
}

esp_err_t aux_udp_link_start(void)
{
    if (s_aux_udp_task) {
        return ESP_OK;
    }

    if (xTaskCreate(aux_udp_task, "aux_udp_tx", 8192, NULL, 2, &s_aux_udp_task) != pdPASS) {
        s_aux_udp_task = NULL;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Aux UDP link task started on port %d", AUX_UDP_PORT);
    return ESP_OK;
}

esp_err_t aux_udp_link_enable_pairing_window(uint32_t duration_sec)
{
    load_pair_state();
    set_pair_mode_window(duration_sec);
    ESP_LOGI(TAG, "Pair mode %s (%u sec)", duration_sec ? "enabled" : "disabled", (unsigned)duration_sec);
    return ESP_OK;
}

esp_err_t aux_udp_link_reset_pairing(void)
{
    load_pair_state();
    memset(&s_pair_state, 0, sizeof(s_pair_state));
    s_have_paired_aux_addr = false;
    set_pair_mode_window(AUX_UDP_PAIR_WINDOW_DEFAULT_SEC);
    ESP_LOGI(TAG, "Pair state reset");
    return persist_pair_state();
}
