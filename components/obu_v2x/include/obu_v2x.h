#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    OBU_V2X_FRAME_NOT_ITS = 0,
    OBU_V2X_FRAME_GEONETWORKING,
    OBU_V2X_FRAME_SECURED,
    OBU_V2X_FRAME_FACILITIES,
} obu_v2x_frame_kind_t;

typedef struct {
    obu_v2x_frame_kind_t kind;
    uint8_t geonetworking_version;
    uint8_t facilities_protocol_version;
    uint8_t message_id;
    uint16_t btp_destination_port;
} obu_v2x_frame_info_t;

/*
 * Classify one complete IEEE 802.11 frame captured by the C5 promiscuous
 * receiver. Returns true for an ETSI GeoNetworking frame. For unsecured
 * GeoNetworking+BTP packets, kind is OBU_V2X_FRAME_FACILITIES when a plausible
 * ItsPduHeader is present and message_id contains its ETSI message identifier.
 * Secured GeoNetworking packets are deliberately not decoded here.
 */
bool obu_v2x_classify_80211_frame(const uint8_t *frame,
                                  size_t frame_len,
                                  obu_v2x_frame_info_t *out);

const char *obu_v2x_message_type_name(uint8_t message_id);

#ifdef __cplusplus
}
#endif
