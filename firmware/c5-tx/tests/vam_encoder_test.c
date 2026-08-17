#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vam_encoder.h"

static int expect_vector(void)
{
    static const uint8_t expected[VAM_MINIMAL_UPER_LEN] = {
        0x03, 0x10, 0x00, 0x00, 0x03, 0xe9, 0x12, 0x34,
        0x00, 0x0a, 0xac, 0x8c, 0x24, 0x03, 0x89, 0xf6,
        0x45, 0x07, 0xff, 0xff, 0xff, 0x08, 0xed, 0xdd,
        0x0f, 0x80, 0x01, 0xc2, 0x7e, 0x07, 0xd3, 0xf2,
        0x83, 0x30,
    };

    const vam_minimal_config_t config = {
        .station_id = 1001,
        .generation_delta_time = 0x1234,
        .station_type = 2,
        .latitude_1e7 = 535600000,
        .longitude_1e7 = 99940000,
        .position_semi_major_confidence_cm = 4095,
        .position_semi_minor_confidence_cm = 4095,
        .position_semi_major_orientation_deg10 = 3601,
        .altitude_cm = 800001,
        .altitude_confidence = 15,
        .heading_deg10 = 900,
        .heading_confidence_deg10 = 127,
        .speed_cm_s = 500,
        .speed_confidence_cm_s = 127,
        .longitudinal_acceleration_dm_s2 = 0,
        .longitudinal_acceleration_confidence_dm_s2 = 102,
    };

    uint8_t encoded[VAM_MINIMAL_UPER_LEN] = {0};
    size_t encoded_len = 0;
    const vam_encode_result_t result =
        vam_encode_minimal_uper(&config, encoded, sizeof(encoded), &encoded_len);
    if (result != VAM_ENCODE_OK) {
        fprintf(stderr, "encode failed: %s\n", vam_encode_result_name(result));
        return 1;
    }
    if (encoded_len != sizeof(expected) || memcmp(encoded, expected, sizeof(expected)) != 0) {
        fprintf(stderr, "encoded VAM differs from the locked UPER test vector\n");
        return 1;
    }
    return 0;
}

static int expect_prohibited_values_rejected(void)
{
    vam_minimal_config_t config = {
        .station_id = 1,
        .generation_delta_time = 0,
        .station_type = 2,
        .latitude_1e7 = 0,
        .longitude_1e7 = 0,
        .position_semi_major_confidence_cm = 4095,
        .position_semi_minor_confidence_cm = 4095,
        .position_semi_major_orientation_deg10 = 3601,
        .altitude_cm = 800001,
        .altitude_confidence = 15,
        .heading_deg10 = 0,
        .heading_confidence_deg10 = 127,
        .speed_cm_s = 0,
        .speed_confidence_cm_s = 127,
        .longitudinal_acceleration_dm_s2 = 161,
        .longitudinal_acceleration_confidence_dm_s2 = 102,
    };
    uint8_t encoded[VAM_MINIMAL_UPER_LEN];
    size_t encoded_len = 0;

    config.heading_deg10 = 3600;
    if (vam_encode_minimal_uper(&config, encoded, sizeof(encoded), &encoded_len) !=
        VAM_ENCODE_VALUE_OUT_OF_RANGE) {
        fprintf(stderr, "prohibited heading 3600 was not rejected\n");
        return 1;
    }

    config.heading_deg10 = 0;
    config.longitude_1e7 = -1800000000;
    if (vam_encode_minimal_uper(&config, encoded, sizeof(encoded), &encoded_len) !=
        VAM_ENCODE_VALUE_OUT_OF_RANGE) {
        fprintf(stderr, "prohibited longitude sentinel was not rejected\n");
        return 1;
    }

    return 0;
}

int main(void)
{
    if (expect_vector() != 0) return 1;
    if (expect_prohibited_values_rejected() != 0) return 1;
    puts("VAM encoder tests passed");
    return 0;
}
