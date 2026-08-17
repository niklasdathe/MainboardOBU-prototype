#include "vam_encoder.h"

#include <stdbool.h>
#include <string.h>

/*
 * This is intentionally a small, deterministic encoder for the exact VAM
 * subset emitted by c5-tx, rather than a general ASN.1 runtime. The bit order
 * and constrained-integer widths follow X.691 Unaligned PER rules.
 */
typedef struct {
    uint8_t *output;
    size_t capacity;
    size_t bit_position;
    bool overflow;
} bit_writer_t;

static unsigned bit_width_u64(uint64_t value)
{
    unsigned bits = 0;
    while (value != 0u) {
        ++bits;
        value >>= 1;
    }
    return bits;
}

static void put_bits(bit_writer_t *writer, uint64_t value, unsigned bit_count)
{
    if (bit_count > 64u || writer->bit_position + bit_count > writer->capacity * 8u) {
        writer->overflow = true;
        return;
    }

    for (unsigned i = 0; i < bit_count; ++i) {
        const unsigned shift = bit_count - 1u - i;
        if (((value >> shift) & 1u) != 0u) {
            writer->output[writer->bit_position >> 3] |=
                (uint8_t)(1u << (7u - (writer->bit_position & 7u)));
        }
        ++writer->bit_position;
    }
}

static bool put_constrained_integer(bit_writer_t *writer,
                                    int64_t value,
                                    int64_t minimum,
                                    int64_t maximum)
{
    if (value < minimum || value > maximum) return false;

    const uint64_t span = (uint64_t)(maximum - minimum);
    const unsigned bit_count = bit_width_u64(span);
    put_bits(writer, (uint64_t)(value - minimum), bit_count);
    return !writer->overflow;
}

#define PUT_CONSTRAINED(writer_, value_, minimum_, maximum_)                     \
    do {                                                                          \
        if (!put_constrained_integer((writer_), (value_), (minimum_), (maximum_))) { \
            return (writer_)->overflow ? VAM_ENCODE_OUTPUT_TOO_SMALL              \
                                        : VAM_ENCODE_VALUE_OUT_OF_RANGE;           \
        }                                                                         \
    } while (0)

vam_encode_result_t vam_encode_minimal_uper(const vam_minimal_config_t *config,
                                             uint8_t *output,
                                             size_t output_capacity,
                                             size_t *output_length)
{
    if (config == NULL || output == NULL || output_length == NULL) {
        return VAM_ENCODE_INVALID_ARGUMENT;
    }
    if (output_capacity < VAM_MINIMAL_UPER_LEN) {
        return VAM_ENCODE_OUTPUT_TOO_SMALL;
    }

    /* Values explicitly prohibited by the CDD must not be emitted. */
    if (config->longitude_1e7 == -1800000000 ||
        config->position_semi_major_orientation_deg10 == 3600u ||
        config->heading_deg10 == 3600u) {
        return VAM_ENCODE_VALUE_OUT_OF_RANGE;
    }

    memset(output, 0, output_capacity);
    bit_writer_t writer = {
        .output = output,
        .capacity = output_capacity,
        .bit_position = 0,
        .overflow = false,
    };

    /* ItsPduHeaderVam. */
    PUT_CONSTRAINED(&writer, 3, 0, 255);                  /* protocolVersion */
    PUT_CONSTRAINED(&writer, 16, 0, 255);                 /* messageId = vam */
    PUT_CONSTRAINED(&writer, config->station_id, 0, UINT32_MAX);

    /* VruAwareness. */
    PUT_CONSTRAINED(&writer, config->generation_delta_time, 0, 65535);

    /*
     * VamParameters is extensible and has four optional root containers after
     * the mandatory BasicContainer and VruHighFrequencyContainer.
     * No extension addition and no optional root container is present.
     */
    put_bits(&writer, 0, 1); /* extension marker */
    put_bits(&writer, 0, 4); /* LF, cluster info, cluster op, motion prediction */

    /* BasicContainer: extension marker + mandatory root fields. */
    put_bits(&writer, 0, 1);
    PUT_CONSTRAINED(&writer, config->station_type, 0, 255);

    /* ReferencePosition. */
    PUT_CONSTRAINED(&writer, config->latitude_1e7, -900000000LL, 900000001LL);
    PUT_CONSTRAINED(&writer, config->longitude_1e7, -1800000000LL, 1800000001LL);
    PUT_CONSTRAINED(&writer, config->position_semi_major_confidence_cm, 0, 4095);
    PUT_CONSTRAINED(&writer, config->position_semi_minor_confidence_cm, 0, 4095);
    PUT_CONSTRAINED(&writer, config->position_semi_major_orientation_deg10, 0, 3601);
    PUT_CONSTRAINED(&writer, config->altitude_cm, -100000, 800001);
    PUT_CONSTRAINED(&writer, config->altitude_confidence, 0, 15);

    /*
     * VruHighFrequencyContainer is extensible and contains eleven optional
     * root fields following the three mandatory values. This minimal encoder
     * leaves all optional fields absent.
     */
    put_bits(&writer, 0, 1);  /* extension marker */
    put_bits(&writer, 0, 11); /* optional root presence bitmap */

    /* Wgs84Angle heading. */
    PUT_CONSTRAINED(&writer, config->heading_deg10, 0, 3601);
    PUT_CONSTRAINED(&writer, config->heading_confidence_deg10, 1, 127);

    /* Speed. */
    PUT_CONSTRAINED(&writer, config->speed_cm_s, 0, 16383);
    PUT_CONSTRAINED(&writer, config->speed_confidence_cm_s, 1, 127);

    /* LongitudinalAcceleration / AccelerationComponent. */
    PUT_CONSTRAINED(&writer, config->longitudinal_acceleration_dm_s2, -160, 161);
    PUT_CONSTRAINED(&writer, config->longitudinal_acceleration_confidence_dm_s2, 0, 102);

    if (writer.overflow) return VAM_ENCODE_OUTPUT_TOO_SMALL;

    *output_length = (writer.bit_position + 7u) / 8u;
    if (*output_length != VAM_MINIMAL_UPER_LEN || writer.bit_position != 269u) {
        return VAM_ENCODE_INVALID_ARGUMENT;
    }
    return VAM_ENCODE_OK;
}

const char *vam_encode_result_name(vam_encode_result_t result)
{
    switch (result) {
    case VAM_ENCODE_OK: return "ok";
    case VAM_ENCODE_INVALID_ARGUMENT: return "invalid argument";
    case VAM_ENCODE_VALUE_OUT_OF_RANGE: return "value out of range/prohibited";
    case VAM_ENCODE_OUTPUT_TOO_SMALL: return "output too small";
    default: return "unknown";
    }
}
