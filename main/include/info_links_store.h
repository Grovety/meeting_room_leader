#ifndef INFO_LINKS_STORE_H
#define INFO_LINKS_STORE_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define INFO_LINKS_STORE_MAX_URL_LEN 512

typedef struct {
    char company_website[INFO_LINKS_STORE_MAX_URL_LEN + 1];
    char office_map[INFO_LINKS_STORE_MAX_URL_LEN + 1];
} info_links_store_data_t;

void info_links_store_get(info_links_store_data_t *out);
void info_links_store_get_company_website_copy(char *out, size_t out_size);
void info_links_store_get_office_map_copy(char *out, size_t out_size);
esp_err_t info_links_store_set(const info_links_store_data_t *data);

#ifdef __cplusplus
}
#endif

#endif
