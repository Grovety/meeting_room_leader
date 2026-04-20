// weather_adapter.cpp - HTTPAdapter implementation (create per-call event group)
#include "weather_adapter.h"
#include <esp_log.h>
#include <esp_http_client.h>
#include <string.h>

static const char* TAG = "http_adapter";

esp_err_t HTTPAdapter::http_event_handler(esp_http_client_event_t* evt) {
    HTTPAdapter* request = reinterpret_cast<HTTPAdapter*>(evt->user_data);

    static bool global_overflow_logged = false;

    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!request) break;
            if (request->buffer_len_ < (request->received_len_ + (uint32_t)evt->data_len)) {
                if (!global_overflow_logged) {
                    ESP_LOGW(TAG, "Buffer too small: have %u, will truncate incoming chunk of %u bytes.", (unsigned)request->buffer_len_, (unsigned)evt->data_len);
                    global_overflow_logged = true;
                }
                if (request->buffer_len_ > request->received_len_) {
                    size_t copyable = request->buffer_len_ - request->received_len_;
                    memcpy(request->buffer_ + request->received_len_, evt->data, copyable);
                    request->received_len_ += copyable;
                }
                break;
            }
            memcpy(request->buffer_ + request->received_len_, evt->data, evt->data_len);
            request->received_len_ += evt->data_len;
            break;

        case HTTP_EVENT_ON_FINISH:
            if (request && request->event_group_) {
                xEventGroupSetBits(request->event_group_, 0x01);
            }
            break;

        default:
            break;
    }
    return ESP_OK;
}

HTTPAdapter::Builder& HTTPAdapter::Builder::setUrl(const char* url) {
    config_.url = url;
    return *this;
}

HTTPAdapter::Builder& HTTPAdapter::Builder::setMethod(esp_http_client_method_t method) {
    config_.method = method;
    return *this;
}

HTTPAdapter::Builder& HTTPAdapter::Builder::setBuffer(char* buffer, uint32_t buffer_len) {
    buffer_ = buffer;
    buffer_len_ = buffer_len;
    return *this;
}

HTTPAdapter::Builder& HTTPAdapter::Builder::setSSL(esp_err_t (*crt_bundle_attach)(void*)) {
    config_.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config_.crt_bundle_attach = crt_bundle_attach;
    return *this;
}

HTTPAdapter::Builder& HTTPAdapter::Builder::setBody(const uint8_t* body, size_t body_size, const char* content_type) {
    body_ = body;
    body_size_ = body_size;
    content_type_ = content_type;
    return *this;
}

HTTPAdapter::Builder& HTTPAdapter::Builder::setAuthHeader(const char* auth_header) {
    auth_header_ = auth_header;
    return *this;
}

HTTPAdapter HTTPAdapter::Builder::build() {
    return HTTPAdapter(config_, buffer_, buffer_len_, body_, body_size_, content_type_, auth_header_);
}

HTTPAdapter::HTTPAdapter(const esp_http_client_config_t& cfg, char* buffer, uint32_t buffer_len,
                        const uint8_t* body, size_t body_size, const char* content_type,
                        const char* auth_header)
    : event_group_(nullptr),
      config_(cfg),
      buffer_(buffer),
      buffer_len_(buffer_len),
      received_len_(0),
      body_(body),
      body_size_(body_size),
      content_type_(content_type),
      auth_header_(auth_header) {

    // Important: handler and user_data are set in perform() (event_group is created there)
    config_.timeout_ms = 15000;
    config_.disable_auto_redirect = false;
    config_.max_redirection_count = 5;
}

HTTPAdapter::~HTTPAdapter() {
    // do not delete anything here; event_group lifecycle is managed in perform()
}

uint32_t HTTPAdapter::perform() {
    received_len_ = 0;
    if (buffer_ && buffer_len_ > 0) {
        buffer_[0] = '\0';
    }

    // Create event_group specific to this call
    event_group_ = xEventGroupCreate();
    if (!event_group_) {
        ESP_LOGE(TAG, "Failed to create event group for HTTP perform");
        return 0;
    }

    // set handler and user_data now, before client initialization
    config_.event_handler = http_event_handler;
    config_.user_data = this;

    esp_http_client_handle_t client = esp_http_client_init(&config_);
    if (!client) {
        ESP_LOGE(TAG, "Failed to init http client");
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
        return 0;
    }

    if (content_type_) {
        esp_http_client_set_header(client, "Content-Type", content_type_);
    }

    if (auth_header_) {
        esp_http_client_set_header(client, "Authorization", auth_header_);
    }

    if (body_ && body_size_ > 0) {
        esp_http_client_set_post_field(client, reinterpret_cast<const char*>(body_), body_size_);
    }

    esp_err_t err = esp_http_client_perform(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
        return 0;
    }

    // Wait until handler marks completion (ON_FINISH)
    xEventGroupWaitBits(event_group_, 0x01, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));

    esp_http_client_cleanup(client);
    vEventGroupDelete(event_group_);
    event_group_ = nullptr;

    return received_len_;
}
