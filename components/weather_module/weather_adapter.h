// weather_adapter.h
#pragma once

#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <esp_http_client.h>

#ifdef __cplusplus
extern "C" {
#endif

class HTTPAdapter {
private:
    static constexpr char Tag[] = "http-adapter";
    
    EventGroupHandle_t       event_group_;
    esp_http_client_config_t config_;
    char*                    buffer_;
    uint32_t                 buffer_len_;
    uint32_t                 received_len_; 
    const uint8_t*           body_;
    size_t                   body_size_;
    const char*              content_type_;
    const char*              auth_header_;

    static esp_err_t http_event_handler(esp_http_client_event_t* evt);

public:
    class Builder {
        esp_http_client_config_t config_ = { 0 };
        char* buffer_ = nullptr;
        uint32_t buffer_len_ = 0;
        const uint8_t* body_ = nullptr;
        size_t body_size_ = 0;
        const char* content_type_ = nullptr;
        const char* auth_header_ = nullptr;

    public:
        Builder& setUrl(const char* url);
        Builder& setMethod(esp_http_client_method_t method);
        Builder& setBuffer(char* buffer, uint32_t buffer_len);
        Builder& setSSL(esp_err_t (*crt_bundle_attach)(void*));
        Builder& setBody(const uint8_t* body, size_t body_size, const char* content_type);
        Builder& setAuthHeader(const char* auth_header);
        
        HTTPAdapter build();
    };

private:
    HTTPAdapter(const esp_http_client_config_t& cfg, char* buffer, uint32_t buffer_len,
                const uint8_t* body, size_t body_size, const char* content_type,
                const char* auth_header);

public:
    ~HTTPAdapter();
    uint32_t perform();
};

#ifdef __cplusplus
}
#endif