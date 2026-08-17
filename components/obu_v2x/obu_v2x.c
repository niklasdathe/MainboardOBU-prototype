#include "obu_v2x.h"

#include <string.h>

#define IEEE80211_TYPE_DATA 2u
#define IEEE80211_FC_TO_DS 0x0100u
#define IEEE80211_FC_FROM_DS 0x0200u
#define IEEE80211_FC_ORDER 0x8000u
#define IEEE80211_QOS_SUBTYPE_BIT 0x08u

#define LLC_SNAP_LEN 8u
#define GEONETWORKING_ETHERTYPE 0x8947u

#define GN_BASIC_HEADER_LEN 4u
#define GN_COMMON_HEADER_LEN 8u
#define GN_BASIC_NEXT_COMMON 1u
#define GN_BASIC_NEXT_SECURED 2u
#define GN_COMMON_NEXT_BTP_A 1u
#define GN_COMMON_NEXT_BTP_B 2u

#define GN_HT_BEACON 0x10u
#define GN_HT_GEOUNICAST 0x20u
#define GN_HT_GEOANYCAST 0x30u
#define GN_HT_GEOBROADCAST 0x40u
#define GN_HT_TSB 0x50u
#define GN_HT_LS 0x60u
#define GN_HT_LS_REPLY 0x61u

#define GN_EXT_BEACON_LEN 24u
#define GN_EXT_GEOUNICAST_LEN 48u
#define GN_EXT_GEOANYCAST_LEN 44u
#define GN_EXT_GEOBROADCAST_LEN 44u
#define GN_EXT_TSB_LEN 28u
#define GN_EXT_LS_REQUEST_LEN 36u
#define GN_EXT_LS_REPLY_LEN 48u

#define BTP_HEADER_LEN 4u
#define ITS_PDU_HEADER_LEN 6u

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool ieee80211_payload_offset(const uint8_t *frame, size_t frame_len, size_t *payload_offset)
{
    if (frame == NULL || payload_offset == NULL || frame_len < 24u) {
        return false;
    }

    const uint16_t fc = (uint16_t)(frame[0] | ((uint16_t)frame[1] << 8));
    const uint8_t type = (uint8_t)((fc >> 2) & 0x03u);
    const uint8_t subtype = (uint8_t)((fc >> 4) & 0x0fu);
    if (type != IEEE80211_TYPE_DATA) {
        return false;
    }

    size_t header_len = 24u;
    if ((fc & (IEEE80211_FC_TO_DS | IEEE80211_FC_FROM_DS)) ==
        (IEEE80211_FC_TO_DS | IEEE80211_FC_FROM_DS)) {
        header_len += 6u;
    }
    if ((subtype & IEEE80211_QOS_SUBTYPE_BIT) != 0u) {
        header_len += 2u;
        if ((fc & IEEE80211_FC_ORDER) != 0u) {
            header_len += 4u;
        }
    }

    if (header_len > frame_len) {
        return false;
    }
    *payload_offset = header_len;
    return true;
}

static bool is_geonetworking_snap(const uint8_t *p, size_t available)
{
    if (p == NULL || available < LLC_SNAP_LEN) {
        return false;
    }
    if (p[0] != 0xaau || p[1] != 0xaau || p[2] != 0x03u ||
        p[3] != 0x00u || p[4] != 0x00u || p[5] != 0x00u) {
        return false;
    }
    return read_be16(p + 6) == GEONETWORKING_ETHERTYPE;
}

static size_t geonetworking_extended_header_len(uint8_t header_type)
{
    switch (header_type & 0xf0u) {
        case GN_HT_BEACON:
            return GN_EXT_BEACON_LEN;
        case GN_HT_GEOUNICAST:
            return GN_EXT_GEOUNICAST_LEN;
        case GN_HT_GEOANYCAST:
            return GN_EXT_GEOANYCAST_LEN;
        case GN_HT_GEOBROADCAST:
            return GN_EXT_GEOBROADCAST_LEN;
        case GN_HT_TSB:
            return GN_EXT_TSB_LEN;
        case GN_HT_LS:
            return header_type == GN_HT_LS_REPLY ? GN_EXT_LS_REPLY_LEN : GN_EXT_LS_REQUEST_LEN;
        default:
            return 0u;
    }
}

const char *obu_v2x_message_type_name(uint8_t message_id)
{
    switch (message_id) {
        case 1: return "DENM";
        case 2: return "CAM";
        case 3: return "POI";
        case 4: return "SPATEM";
        case 5: return "MAPEM";
        case 6: return "IVIM";
        case 7: return "EV-RSR";
        case 8: return "TISTPG";
        case 9: return "SREM";
        case 10: return "SSEM";
        case 11: return "EVCSN";
        case 12: return "SAEM";
        case 13: return "RTCMEM";
        case 14: return "CPM";
        case 15: return "IMZM";
        case 16: return "VAM";
        case 17: return "DSM";
        case 18: return "PCIM";
        case 19: return "PCVM";
        case 20: return "MCM";
        case 21: return "PAM";
        default: return "UNKNOWN";
    }
}

bool obu_v2x_classify_80211_frame(const uint8_t *frame,
                                  size_t frame_len,
                                  obu_v2x_frame_info_t *out)
{
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->kind = OBU_V2X_FRAME_NOT_ITS;

    size_t mac_payload_offset;
    if (!ieee80211_payload_offset(frame, frame_len, &mac_payload_offset) ||
        frame_len - mac_payload_offset < LLC_SNAP_LEN ||
        !is_geonetworking_snap(frame + mac_payload_offset, frame_len - mac_payload_offset)) {
        return false;
    }

    const size_t gn_offset = mac_payload_offset + LLC_SNAP_LEN;
    if (frame_len - gn_offset < GN_BASIC_HEADER_LEN) {
        return false;
    }

    const uint8_t basic_first = frame[gn_offset];
    const uint8_t basic_next_header = (uint8_t)(basic_first & 0x0fu);
    out->geonetworking_version = (uint8_t)(basic_first >> 4);
    out->kind = OBU_V2X_FRAME_GEONETWORKING;

    if (basic_next_header == GN_BASIC_NEXT_SECURED) {
        out->kind = OBU_V2X_FRAME_SECURED;
        return true;
    }
    if (basic_next_header != GN_BASIC_NEXT_COMMON ||
        frame_len - gn_offset < GN_BASIC_HEADER_LEN + GN_COMMON_HEADER_LEN) {
        return true;
    }

    const size_t common_offset = gn_offset + GN_BASIC_HEADER_LEN;
    const uint8_t common_next_header = (uint8_t)(frame[common_offset] >> 4);
    const uint8_t header_type = frame[common_offset + 1u];
    const uint16_t payload_len = read_be16(frame + common_offset + 4u);
    const size_t ext_len = geonetworking_extended_header_len(header_type);
    if (ext_len == 0u ||
        (common_next_header != GN_COMMON_NEXT_BTP_A && common_next_header != GN_COMMON_NEXT_BTP_B)) {
        return true;
    }

    const size_t gn_header_len = GN_BASIC_HEADER_LEN + GN_COMMON_HEADER_LEN + ext_len;
    if (payload_len < BTP_HEADER_LEN + ITS_PDU_HEADER_LEN ||
        gn_header_len > frame_len - gn_offset ||
        payload_len > frame_len - gn_offset - gn_header_len) {
        return true;
    }

    const size_t btp_offset = gn_offset + gn_header_len;
    const size_t facilities_offset = btp_offset + BTP_HEADER_LEN;
    out->btp_destination_port = read_be16(frame + btp_offset);
    out->facilities_protocol_version = frame[facilities_offset];
    out->message_id = frame[facilities_offset + 1u];

    /* Current ETSI facilities PDUs use small non-zero protocol versions and
     * message identifiers allocated in the common MessageId registry. Keep the
     * validation conservative so arbitrary BTP application data is not labelled
     * as CAM/DENM/etc. */
    if (out->facilities_protocol_version >= 1u && out->facilities_protocol_version <= 3u &&
        out->message_id >= 1u && out->message_id <= 21u) {
        out->kind = OBU_V2X_FRAME_FACILITIES;
    }

    return true;
}
