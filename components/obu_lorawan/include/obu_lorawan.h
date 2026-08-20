#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBU_LORAWAN_FRAGMENT_PROTOCOL_VERSION 1U
#define OBU_LORAWAN_FRAGMENT_HEADER_BYTES 12U
#define OBU_LORAWAN_MAX_FRAME_BYTES_LIMIT 512U
/* EU868 DR4/DR5 allow a 242-byte LoRaWAN application payload in RadioLib.
 * Reserve 12 bytes for the BicycleOBU fragment header, leaving 230 raw bytes.
 * The worker clamps this further at runtime through getMaxPayloadLen() when a
 * lower data rate or current MAC state requires a smaller payload. */
#define OBU_LORAWAN_MAX_FRAGMENT_DATA_BYTES 230U

typedef struct obu_lorawan obu_lorawan_t;

typedef struct {
    bool enabled;
    spi_host_device_t host;
    int sck_gpio;
    int miso_gpio;
    int mosi_gpio;
    int nss_gpio;
    int dio1_gpio;
    int reset_gpio;
    int busy_gpio;
    uint32_t spi_clock_hz;
    const char *join_eui_hex;
    const char *dev_eui_hex;
    const char *nwk_key_hex;
    const char *app_key_hex;
    uint8_t join_datarate;
    uint8_t fport;
    uint16_t max_frame_bytes;
    uint8_t fragment_data_bytes;
    uint8_t queue_depth;
    uint32_t min_fragment_interval_ms;
    uint32_t join_retry_ms;
} obu_lorawan_config_t;

typedef struct {
    uint32_t frames_offered;
    uint32_t frames_queued;
    uint32_t frames_sent;
    uint32_t frames_dropped_queue;
    uint32_t frames_dropped_oversize;
    uint32_t fragments_sent;
    uint32_t tx_errors;
    uint32_t join_attempts;
    uint32_t join_failures;
    bool joined;
    bool link_metrics_valid;
    float last_downlink_rssi_dbm;
    float last_downlink_snr_db;
} obu_lorawan_stats_t;

/**
 * Start the SX1262/LoRaWAN worker.
 *
 * The worker owns the RadioLib radio/node objects and all LoRaWAN timing.
 * The caller may continue acquisition even when the radio is disabled,
 * credentials are missing, or the network is unavailable.
 */
esp_err_t obu_lorawan_start(const obu_lorawan_config_t *config, obu_lorawan_t **out);

/**
 * Queue one raw C-ITS frame for best-effort LoRaWAN forwarding.
 *
 * This is intentionally non-blocking. If the bounded queue is full, the oldest
 * pending frame is discarded so current traffic can still be sampled.
 */
esp_err_t obu_lorawan_enqueue_frame(obu_lorawan_t *uplink, const uint8_t *frame, size_t frame_len);

void obu_lorawan_get_stats(const obu_lorawan_t *uplink, obu_lorawan_stats_t *out);

#ifdef __cplusplus
}
#endif
