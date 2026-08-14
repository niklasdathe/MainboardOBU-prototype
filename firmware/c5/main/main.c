#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "obu_core.h"
#include "obu_ipc.h"
#include "obu_radio.h"

static const char *TAG = "c5_main";
static obu_ipc_endpoint_t *ipc;
static obu_radio_t *radio;
static uint32_t ipc_seq;
static uint32_t status_count;
static uint32_t s3_message_count;
static uint32_t time_probe_count;
static uint32_t radio_rx_count;
static bool s3_seen;

static obu_ipc_message_t status_message;
static obu_ipc_message_t radio_rx_message;
static obu_ipc_message_t tx_result_message;
static obu_ipc_message_t time_response_message;
static obu_ipc_message_t main_rx_message;

typedef struct __attribute__((packed)) {
    obu_v2x_rx_meta_t meta;
    uint16_t frame_len;
} rx_wire_t;

typedef struct __attribute__((packed)) {
    uint32_t request_id;
    uint16_t frame_len;
    uint16_t flags;
} tx_req_t;

typedef struct __attribute__((packed)) {
    uint32_t request_id;
    int32_t result;
    uint16_t frame_len;
    uint16_t reserved;
} tx_res_t;

typedef struct __attribute__((packed)) {
    uint16_t frequency_mhz;
    uint8_t enable;
    uint8_t reserved;
} radio_cfg_t;

typedef struct __attribute__((packed)) {
    uint32_t nonce;
    uint8_t arm;
    uint8_t reserved[3];
} arm_t;

static const char *ipc_type_name(obu_ipc_type_t type)
{
    switch (type) {
        case OBU_IPC_NOP: return "NOP";
        case OBU_IPC_RX_FRAME: return "RX_FRAME";
        case OBU_IPC_RADIO_STATUS: return "RADIO_STATUS";
        case OBU_IPC_TX_REQUEST: return "TX_REQUEST";
        case OBU_IPC_TX_RESULT: return "TX_RESULT";
        case OBU_IPC_SET_RADIO: return "SET_RADIO";
        case OBU_IPC_ARM_TX: return "ARM_TX";
        case OBU_IPC_TIME_PROBE: return "TIME_PROBE";
        case OBU_IPC_TIME_RESPONSE: return "TIME_RESPONSE";
        case OBU_IPC_ACK: return "ACK";
        default: return "UNKNOWN";
    }
}

static void send_status(void)
{
    obu_radio_status_t s;
    obu_radio_get_status(radio, &s);
    s.ipc_drop = obu_ipc_rx_queue_drops(ipc) + obu_ipc_tx_queue_drops(ipc);

    memset(&status_message, 0, sizeof(status_message));
    status_message.type = OBU_IPC_RADIO_STATUS;
    status_message.sequence = ipc_seq++;
    status_message.source_monotonic_us = obu_monotonic_us();
    status_message.payload_len = sizeof(s);
    memcpy(status_message.payload, &s, sizeof(s));

    const esp_err_t err = obu_ipc_send(ipc, &status_message, 0);
    status_count++;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to queue radio status #%u: %s", (unsigned)status_count, esp_err_to_name(err));
    } else if (status_count == 1u || (status_count % 5u) == 0u) {
        ESP_LOGI(TAG,
                 "Radio status #%u queued: radio=%s tx_armed=%s freq=%uMHz rx=%u drop_buf=%u drop_size=%u ipc_drop=%u tx=%u/%u failed=%u",
                 (unsigned)status_count,
                 s.radio_running ? "ON" : "OFF",
                 s.tx_armed ? "YES" : "NO",
                 (unsigned)s.frequency_mhz,
                 (unsigned)s.rx_frames,
                 (unsigned)s.rx_drop_no_buffer,
                 (unsigned)s.rx_drop_oversize,
                 (unsigned)s.ipc_drop,
                 (unsigned)s.tx_success,
                 (unsigned)s.tx_requests,
                 (unsigned)s.tx_failed);
    }
}

static void on_rx(const obu_v2x_rx_meta_t *meta, const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (sizeof(rx_wire_t) + len > OBU_IPC_PAYLOAD_MAX) {
        ESP_LOGW(TAG, "RX frame too large for IPC: %u", (unsigned)len);
        return;
    }

    radio_rx_count++;
    if (radio_rx_count == 1u || (radio_rx_count % 100u) == 0u) {
        ESP_LOGI(TAG, "V2X radio RX #%u: seq=%u len=%u freq=%uMHz rssi=%ddBm hw_ts=%llu",
                 (unsigned)radio_rx_count,
                 (unsigned)meta->c5_sequence,
                 (unsigned)len,
                 (unsigned)meta->frequency_mhz,
                 (int)meta->rssi_dbm,
                 (unsigned long long)meta->radio_hw_timestamp_us);
    }

    memset(&radio_rx_message, 0, sizeof(radio_rx_message));
    radio_rx_message.type = OBU_IPC_RX_FRAME;
    radio_rx_message.sequence = ipc_seq++;
    radio_rx_message.source_monotonic_us = meta->c5_rx_monotonic_us;
    radio_rx_message.payload_len = (uint16_t)(sizeof(rx_wire_t) + len);

    rx_wire_t wire = {.meta = *meta, .frame_len = (uint16_t)len};
    memcpy(radio_rx_message.payload, &wire, sizeof(wire));
    memcpy(radio_rx_message.payload + sizeof(wire), frame, len);

    const esp_err_t err = obu_ipc_send(ipc, &radio_rx_message, 0);
    if (err != ESP_OK) ESP_LOGW(TAG, "Failed to queue V2X RX frame #%u to S3: %s", (unsigned)radio_rx_count, esp_err_to_name(err));
}

static void mark_s3_message(const obu_ipc_message_t *m)
{
    s3_message_count++;
    if (!s3_seen) {
        s3_seen = true;
        ESP_LOGI(TAG, "S3 application IPC active: first message type=%s(%u) seq=%u",
                 ipc_type_name(m->type), (unsigned)m->type, (unsigned)m->sequence);
    }
    ESP_LOGD(TAG, "S3 IPC RX type=%s(%u) seq=%u payload=%u",
             ipc_type_name(m->type), (unsigned)m->type, (unsigned)m->sequence, (unsigned)m->payload_len);
}

static void handle(const obu_ipc_message_t *m)
{
    mark_s3_message(m);

    if (m->type == OBU_IPC_SET_RADIO && m->payload_len >= sizeof(radio_cfg_t)) {
        radio_cfg_t cfg;
        memcpy(&cfg, m->payload, sizeof(cfg));
        ESP_LOGI(TAG, "S3 SET_RADIO: enable=%u frequency=%uMHz", (unsigned)cfg.enable, (unsigned)cfg.frequency_mhz);
        if (cfg.enable) {
            const esp_err_t start_err = obu_radio_start(radio);
            if (start_err != ESP_OK) ESP_LOGW(TAG, "Radio start request failed: %s", esp_err_to_name(start_err));
        }
        const esp_err_t freq_err = obu_radio_set_frequency(radio, cfg.frequency_mhz);
        if (freq_err != ESP_OK) ESP_LOGW(TAG, "Radio frequency request failed: %s", esp_err_to_name(freq_err));
        send_status();
    } else if (m->type == OBU_IPC_ARM_TX && m->payload_len >= sizeof(arm_t)) {
        arm_t arm;
        memcpy(&arm, m->payload, sizeof(arm));
        const esp_err_t err = obu_radio_arm_tx(radio, arm.nonce, arm.arm != 0);
        ESP_LOGI(TAG, "S3 ARM_TX: arm=%u nonce=%u result=%s", (unsigned)arm.arm, (unsigned)arm.nonce, esp_err_to_name(err));
        send_status();
    } else if (m->type == OBU_IPC_TX_REQUEST && m->payload_len >= sizeof(tx_req_t)) {
        tx_req_t request;
        memcpy(&request, m->payload, sizeof(request));
        esp_err_t result = ESP_ERR_INVALID_SIZE;
        if (sizeof(request) + request.frame_len <= m->payload_len) {
            result = obu_radio_transmit(radio, m->payload + sizeof(request), request.frame_len);
        }
        ESP_LOGI(TAG, "S3 TX_REQUEST id=%u len=%u result=%s",
                 (unsigned)request.request_id, (unsigned)request.frame_len, esp_err_to_name(result));

        tx_res_t response = {
            .request_id = request.request_id,
            .result = result,
            .frame_len = request.frame_len,
        };
        memset(&tx_result_message, 0, sizeof(tx_result_message));
        tx_result_message.type = OBU_IPC_TX_RESULT;
        tx_result_message.sequence = ipc_seq++;
        tx_result_message.source_monotonic_us = obu_monotonic_us();
        tx_result_message.payload_len = sizeof(response);
        memcpy(tx_result_message.payload, &response, sizeof(response));
        const esp_err_t send_err = obu_ipc_send(ipc, &tx_result_message, 0);
        if (send_err != ESP_OK) ESP_LOGW(TAG, "Failed to queue TX_RESULT: %s", esp_err_to_name(send_err));
    } else if (m->type == OBU_IPC_TIME_PROBE && m->payload_len >= 8) {
        uint64_t t1;
        memcpy(&t1, m->payload, 8);
        const uint64_t t2 = obu_monotonic_us();
        memset(&time_response_message, 0, sizeof(time_response_message));
        time_response_message.type = OBU_IPC_TIME_RESPONSE;
        time_response_message.sequence = ipc_seq++;
        time_response_message.source_monotonic_us = t2;
        time_response_message.payload_len = 24;
        memcpy(time_response_message.payload, &t1, 8);
        memcpy(time_response_message.payload + 8, &t2, 8);
        const uint64_t t3 = obu_monotonic_us();
        memcpy(time_response_message.payload + 16, &t3, 8);
        const esp_err_t err = obu_ipc_send(ipc, &time_response_message, 0);
        time_probe_count++;
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to queue TIME_RESPONSE #%u: %s", (unsigned)time_probe_count, esp_err_to_name(err));
        } else if (time_probe_count == 1u || (time_probe_count % 5u) == 0u) {
            ESP_LOGI(TAG, "S3 time probes handled=%u total_s3_messages=%u", (unsigned)time_probe_count, (unsigned)s3_message_count);
        }
    } else {
        ESP_LOGW(TAG, "Unhandled/malformed S3 IPC message: type=%s(%u) payload=%u",
                 ipc_type_name(m->type), (unsigned)m->type, (unsigned)m->payload_len);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG,
             "C5 SPI wiring expected: C5 GPIO8 SCLK <- S3 GPIO7; C5 GPIO10 MOSI <- S3 GPIO9; C5 GPIO9 MISO -> S3 GPIO8; C5 GPIO1 CS <- S3 GPIO1; common GND; 8MHz mode0");

    obu_ipc_config_t ipc_config = {
        .role = OBU_IPC_ROLE_C5_SLAVE,
        .host = SPI2_HOST,
        .gpio_sclk = 8,
        .gpio_miso = 9,
        .gpio_mosi = 10,
        .gpio_cs = 1,
        .gpio_data_ready = -1,
        .queue_depth = 12,
        .clock_hz = 8000000,
    };
    ESP_ERROR_CHECK(obu_ipc_init(&ipc_config, &ipc));
    ESP_LOGI(TAG, "C5 IPC slave initialized; waiting for S3 SPI clocks");

    obu_radio_config_t radio_config = {
        .frequency_mhz = 5900,
        .rx_cb = on_rx,
    };
    ESP_ERROR_CHECK(obu_radio_create(&radio_config, &radio));
    ESP_ERROR_CHECK(obu_radio_start(radio));
    ESP_LOGI(TAG, "C5 V2X radio started at 5900 MHz");

    send_status();
    uint64_t last_status_us = 0;
    for (;;) {
        if (obu_ipc_receive(ipc, &main_rx_message, pdMS_TO_TICKS(250)) == ESP_OK) handle(&main_rx_message);
        const uint64_t now = obu_monotonic_us();
        if (now - last_status_us > 1000000ULL) {
            send_status();
            last_status_us = now;
        }
    }
}
