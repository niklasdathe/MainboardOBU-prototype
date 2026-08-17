#pragma once

#include <stddef.h>
#include <stdint.h>

#define VAM_MINIMAL_UPER_LEN 34u

typedef struct {
    uint32_t station_id;
    uint16_t generation_delta_time;
    uint8_t station_type;

    int32_t latitude_1e7;
    int32_t longitude_1e7;
    uint16_t position_semi_major_confidence_cm;
    uint16_t position_semi_minor_confidence_cm;
    uint16_t position_semi_major_orientation_deg10;
    int32_t altitude_cm;
    uint8_t altitude_confidence;

    uint16_t heading_deg10;
    uint8_t heading_confidence_deg10;
    uint16_t speed_cm_s;
    uint8_t speed_confidence_cm_s;
    int16_t longitudinal_acceleration_dm_s2;
    uint8_t longitudinal_acceleration_confidence_dm_s2;
} vam_minimal_config_t;

typedef enum {
    VAM_ENCODE_OK = 0,
    VAM_ENCODE_INVALID_ARGUMENT,
    VAM_ENCODE_VALUE_OUT_OF_RANGE,
    VAM_ENCODE_OUTPUT_TOO_SMALL,
} vam_encode_result_t;

/*
 * Encode the ETSI TS 103 300-3 Release-2 VAM root fields used by the
 * BicycleOBU test transmitter with Unaligned PER (UPER).
 *
 * Generated structure:
 *   VAM
 *     ItsPduHeaderVam(protocolVersion=3, messageId=16, stationId)
 *     VruAwareness
 *       generationDeltaTime
 *       VamParameters
 *         BasicContainer
 *         VruHighFrequencyContainer
 *
 * Optional VAM, BasicContainer and high-frequency extension fields are absent.
 * The encoded PDU is 269 bits and therefore occupies 34 octets after final
 * zero padding of the last octet.
 */
vam_encode_result_t vam_encode_minimal_uper(const vam_minimal_config_t *config,
                                             uint8_t *output,
                                             size_t output_capacity,
                                             size_t *output_length);

const char *vam_encode_result_name(vam_encode_result_t result);
