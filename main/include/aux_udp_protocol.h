#ifndef AUX_UDP_PROTOCOL_H
#define AUX_UDP_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUX_UDP_PORT 33334

#define AUX_UDP_MAGIC   0x4D435542u /* "MCUB" */
#define AUX_UDP_VERSION 1u

#define AUX_UDP_MSG_STATE     1u
#define AUX_UDP_MSG_PAIR_REQ  2u
#define AUX_UDP_MSG_PAIR_ACK  3u
#define AUX_UDP_MSG_PAIR_NACK 4u
#define AUX_UDP_MSG_BEACON    5u

#define AUX_UDP_FLAG_MAIN_PAIRED    0x01u
#define AUX_UDP_FLAG_MAIN_PAIR_MODE 0x02u

#define AUX_UDP_MAIN_ID_LEN        16
#define AUX_UDP_AUX_ID_LEN         16
#define AUX_UDP_PAIR_TOKEN_LEN     33
#define AUX_UDP_ROOM_NAME_LEN      48
#define AUX_UDP_CLOCK_TEXT_LEN     16
#define AUX_UDP_TIME_RANGE_LEN     24
#define AUX_UDP_NEXT_DATE_LEN      32
#define AUX_UDP_NEXT_TITLE_LEN     128
#define AUX_UDP_NEXT_BOOKED_BY_LEN 128

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t packet_size;
    uint8_t msg_type;
    uint8_t flags;
    uint16_t reserved0;
    uint32_t seq;
    uint32_t state_rev;
    int64_t sent_epoch_sec;

    char main_id[AUX_UDP_MAIN_ID_LEN];
    char aux_id[AUX_UDP_AUX_ID_LEN];
    char pair_token[AUX_UDP_PAIR_TOKEN_LEN];

    uint8_t occupied;
    uint8_t has_next_booking;
    uint8_t reserved[2];
    int32_t remaining_sec;

    char room_name[AUX_UDP_ROOM_NAME_LEN];
    char clock_text[AUX_UDP_CLOCK_TEXT_LEN];
    char next_time_range[AUX_UDP_TIME_RANGE_LEN];
    char next_date_text[AUX_UDP_NEXT_DATE_LEN];
    char next_title[AUX_UDP_NEXT_TITLE_LEN];
    char next_booked_by[AUX_UDP_NEXT_BOOKED_BY_LEN];

    uint32_t crc32;
} aux_udp_packet_v1_t;
#pragma pack(pop)

#ifdef __cplusplus
}
#endif

#endif
