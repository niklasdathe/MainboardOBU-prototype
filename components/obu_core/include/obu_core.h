#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBU_EVENT_PAYLOAD_MAX 2464u
#define OBU_BUS_MAX_SUBSCRIBERS 8u

typedef enum {
    OBU_SOURCE_UNKNOWN=0, OBU_SOURCE_C5_RADIO, OBU_SOURCE_GNSS, OBU_SOURCE_CAN,
    OBU_SOURCE_BLE_SENSOR, OBU_SOURCE_PHONE, OBU_SOURCE_DERIVED, OBU_SOURCE_SYSTEM
} obu_source_t;

typedef enum {
    OBU_DATA_NONE=0, OBU_DATA_V2X_RAW_RX, OBU_DATA_V2X_TX_RESULT, OBU_DATA_RADIO_STATUS,
    OBU_DATA_GNSS_FIX, OBU_DATA_GNSS_NMEA, OBU_DATA_CAN_FRAME, OBU_DATA_BLE_SAMPLE,
    OBU_DATA_WARNING, OBU_DATA_HMI_STATE, OBU_DATA_CLOCK_SYNC, OBU_DATA_DIAGNOSTIC
} obu_data_type_t;

typedef enum { OBU_TIME_UNSYNCED=0, OBU_TIME_RTC_HOLDOVER, OBU_TIME_GNSS, OBU_TIME_GNSS_PPS } obu_time_quality_t;

enum {
    OBU_EVENT_F_RAW=1u<<0, OBU_EVENT_F_DERIVED=1u<<1, OBU_EVENT_F_VALID=1u<<2,
    OBU_EVENT_F_STALE=1u<<3, OBU_EVENT_F_SELF_TX=1u<<4, OBU_EVENT_F_TRUNCATED=1u<<5
};

typedef struct {
    obu_source_t source;
    obu_data_type_t type;
    uint32_t sequence;
    uint32_t flags;
    uint64_t source_monotonic_us;
    uint64_t hub_monotonic_us;
    int64_t utc_ns;
    obu_time_quality_t time_quality;
    uint32_t validity_ms;
    uint16_t payload_len;
    uint8_t payload[OBU_EVENT_PAYLOAD_MAX];
} obu_event_t;

typedef struct {
    QueueHandle_t queues[OBU_BUS_MAX_SUBSCRIBERS];
    uint32_t subscriber_drops[OBU_BUS_MAX_SUBSCRIBERS];
    uint8_t count;
    portMUX_TYPE lock;
} obu_bus_t;

typedef struct {
    int32_t latitude_e7;
    int32_t longitude_e7;
    int32_t altitude_mm;
    uint32_t speed_mm_s;
    uint32_t course_cdeg;
    uint16_t hdop_centi;
    uint8_t satellites;
    uint8_t fix_quality;
    bool position_valid;
    bool velocity_valid;
    bool utc_valid;
    int64_t utc_ns;
} obu_gnss_fix_t;

typedef struct {
    uint16_t frequency_mhz;
    int8_t rssi_dbm;
    uint8_t wifi_packet_type;
    uint8_t rx_state;
    uint16_t original_len;
    uint16_t captured_len;
    uint32_t c5_sequence;
    uint64_t radio_hw_timestamp_us;
    uint64_t c5_rx_monotonic_us;
} obu_v2x_rx_meta_t;

typedef struct {
    bool radio_running;
    bool tx_armed;
    uint16_t frequency_mhz;
    uint32_t boot_nonce;
    uint32_t rx_frames;
    uint32_t rx_drop_no_buffer;
    uint32_t rx_drop_oversize;
    uint32_t ipc_drop;
    uint32_t tx_requests;
    uint32_t tx_success;
    uint32_t tx_failed;
    uint32_t reset_reason;
    char firmware_version[32];
} obu_radio_status_t;

typedef struct {
    int64_t c5_to_s3_offset_us;
    uint32_t last_rtt_us;
    uint32_t samples;
    bool valid;
} obu_clock_sync_status_t;

typedef struct {
    float x_m;
    float y_m;
    float z_m;
    float q_w;
    float q_x;
    float q_y;
    float q_z;
    uint32_t calibration_revision;
    int64_t valid_from_utc_ns;
} obu_sensor_pose_t;

typedef struct {
    uint32_t id;
    bool extended;
    bool rtr;
    uint8_t dlc;
    uint8_t data[8];
} obu_can_frame_t;

void obu_bus_init(obu_bus_t *bus);
esp_err_t obu_bus_subscribe(obu_bus_t *bus, QueueHandle_t queue, uint8_t *subscriber_id);
esp_err_t obu_bus_publish(obu_bus_t *bus, const obu_event_t *event);
uint32_t obu_bus_subscriber_drops(const obu_bus_t *bus, uint8_t subscriber_id);
uint64_t obu_monotonic_us(void);

#ifdef __cplusplus
}
#endif
