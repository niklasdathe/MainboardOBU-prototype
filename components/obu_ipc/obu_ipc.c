#include "obu_ipc.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define HDR_LEN 32u
#define CRC_OFFSET 28u
#define CRC_LEN 4u
#define IPC_POLL_INTERVAL_MS 20u
#define IPC_RESPONSE_BURST_POLLS 3u
#define IPC_RESPONSE_BURST_DELAY_MS 1u
#define IPC_SLAVE_PEER_FRESH_US 250000ULL
#define IPC_SLAVE_STALE_TX_US 2000000ULL
#define IPC_TASK_STACK_BYTES 6144u
#define IPC_DIAG_INTERVAL_US 5000000ULL

static const char *TAG = "obu_ipc";

struct obu_ipc_endpoint {
    obu_ipc_config_t cfg;
    QueueHandle_t txq;
    QueueHandle_t rxq;
    TaskHandle_t task;
    spi_device_handle_t master_dev;
    uint32_t crc_errors;
    uint32_t queue_drops;
    uint32_t tx_queue_drops;
    uint32_t stale_tx_drops;
    uint32_t transfer_count;
    uint32_t valid_rx_count;
    uint32_t invalid_frame_count;
    uint32_t spi_errors;
    obu_ipc_type_t last_rx_type;
    uint64_t last_valid_rx_us;
    uint64_t last_diag_us;
    bool peer_seen;
    obu_ipc_message_t tx_scratch;
    obu_ipc_message_t rx_scratch;
};

static const char *role_name(const obu_ipc_endpoint_t *ep)
{
    return ep->cfg.role == OBU_IPC_ROLE_S3_MASTER ? "S3-master" : "C5-slave";
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)rd16(p) | ((uint32_t)rd16(p + 2) << 16);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void wr32(uint8_t *p, uint32_t value)
{
    wr16(p, (uint16_t)value);
    wr16(p + 2, (uint16_t)(value >> 16));
}

static void wr64(uint8_t *p, uint64_t value)
{
    wr32(p, (uint32_t)value);
    wr32(p + 4, (uint32_t)(value >> 32));
}

static uint32_t crc32_ieee_frame(const uint8_t *data, size_t len)
{
    uint32_t crc = ~0u;
    for (size_t i = 0; i < len; ++i) {
        const uint8_t byte = (i >= CRC_OFFSET && i < CRC_OFFSET + CRC_LEN) ? 0u : data[i];
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static void prepare_nop(obu_ipc_message_t *msg, uint32_t sequence)
{
    memset(msg, 0, sizeof(*msg));
    msg->type = OBU_IPC_NOP;
    msg->sequence = sequence;
    msg->source_monotonic_us = (uint64_t)esp_timer_get_time();
}

static void log_link_stats(obu_ipc_endpoint_t *ep)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (ep->last_diag_us != 0 && now - ep->last_diag_us < IPC_DIAG_INTERVAL_US) return;
    ep->last_diag_us = now;

    if (ep->last_valid_rx_us != 0) {
        ESP_LOGI(TAG,
                 "%s link: transfers=%u valid_rx=%u invalid=%u crc=%u spi_err=%u rxq_drop=%u txq_drop=%u stale_tx=%u last_type=%u age=%llu ms",
                 role_name(ep),
                 (unsigned)ep->transfer_count,
                 (unsigned)ep->valid_rx_count,
                 (unsigned)ep->invalid_frame_count,
                 (unsigned)ep->crc_errors,
                 (unsigned)ep->spi_errors,
                 (unsigned)ep->queue_drops,
                 (unsigned)ep->tx_queue_drops,
                 (unsigned)ep->stale_tx_drops,
                 (unsigned)ep->last_rx_type,
                 (unsigned long long)((now - ep->last_valid_rx_us) / 1000ULL));
    } else {
        ESP_LOGW(TAG,
                 "%s link: no valid peer frame yet; transfers=%u invalid=%u crc=%u spi_err=%u rxq_drop=%u txq_drop=%u stale_tx=%u",
                 role_name(ep),
                 (unsigned)ep->transfer_count,
                 (unsigned)ep->invalid_frame_count,
                 (unsigned)ep->crc_errors,
                 (unsigned)ep->spi_errors,
                 (unsigned)ep->queue_drops,
                 (unsigned)ep->tx_queue_drops,
                 (unsigned)ep->stale_tx_drops);
    }
}

esp_err_t obu_ipc_encode(const obu_ipc_message_t *msg,
                         uint8_t *out,
                         size_t out_cap,
                         size_t *out_len)
{
    if (msg == NULL || out == NULL || msg->payload_len > OBU_IPC_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;

    const size_t encoded_len = HDR_LEN + msg->payload_len;
    if (out_cap < encoded_len) return ESP_ERR_INVALID_SIZE;

    memset(out, 0, encoded_len);
    wr32(out, OBU_IPC_MAGIC);
    out[4] = OBU_IPC_VERSION;
    out[5] = (uint8_t)msg->type;
    wr16(out + 6, HDR_LEN);
    wr16(out + 8, msg->payload_len);
    wr32(out + 12, msg->sequence);
    wr32(out + 16, msg->flags);
    wr64(out + 20, msg->source_monotonic_us);
    if (msg->payload_len > 0) memcpy(out + HDR_LEN, msg->payload, msg->payload_len);

    wr32(out + CRC_OFFSET, crc32_ieee_frame(out, encoded_len));
    if (out_len != NULL) *out_len = encoded_len;
    return ESP_OK;
}

esp_err_t obu_ipc_decode(const uint8_t *buf, size_t len, obu_ipc_message_t *msg)
{
    if (buf == NULL || msg == NULL || len < HDR_LEN) return ESP_ERR_INVALID_SIZE;
    if (rd32(buf) != OBU_IPC_MAGIC || buf[4] != OBU_IPC_VERSION || rd16(buf + 6) != HDR_LEN) {
        return ESP_ERR_INVALID_VERSION;
    }

    const uint16_t payload_len = rd16(buf + 8);
    const size_t encoded_len = HDR_LEN + payload_len;
    if (payload_len > OBU_IPC_PAYLOAD_MAX || encoded_len > len) return ESP_ERR_INVALID_SIZE;

    const uint32_t expected_crc = rd32(buf + CRC_OFFSET);
    if (crc32_ieee_frame(buf, encoded_len) != expected_crc) return ESP_ERR_INVALID_CRC;

    memset(msg, 0, sizeof(*msg));
    msg->type = (obu_ipc_type_t)buf[5];
    msg->payload_len = payload_len;
    msg->sequence = rd32(buf + 12);
    msg->flags = rd32(buf + 16);
    msg->source_monotonic_us = rd64(buf + 20);
    if (payload_len > 0) memcpy(msg->payload, buf + HDR_LEN, payload_len);
    return ESP_OK;
}

static void enqueue_rx(obu_ipc_endpoint_t *ep, const uint8_t *buf, size_t len)
{
    obu_ipc_message_t *msg = &ep->rx_scratch;
    const esp_err_t err = obu_ipc_decode(buf, len, msg);
    if (err == ESP_ERR_INVALID_CRC) {
        ep->crc_errors++;
        return;
    }
    if (err != ESP_OK) {
        ep->invalid_frame_count++;
        return;
    }

    const uint64_t now = (uint64_t)esp_timer_get_time();
    const bool recovered_after_gap = ep->last_valid_rx_us != 0 && now - ep->last_valid_rx_us > 1000000ULL;
    ep->valid_rx_count++;
    ep->last_rx_type = msg->type;
    ep->last_valid_rx_us = now;
    if (!ep->peer_seen) {
        ep->peer_seen = true;
        ESP_LOGI(TAG, "%s IPC peer responded with valid frame type=%u seq=%u",
                 role_name(ep), (unsigned)msg->type, (unsigned)msg->sequence);
    } else if (recovered_after_gap) {
        ESP_LOGI(TAG, "%s IPC peer recovered after link gap; type=%u seq=%u",
                 role_name(ep), (unsigned)msg->type, (unsigned)msg->sequence);
    }

    if (msg->type == OBU_IPC_NOP) return;
    if (xQueueSend(ep->rxq, msg, 0) != pdTRUE) {
        ep->queue_drops++;
        ESP_LOGW(TAG, "%s RX queue full; drop type=%u seq=%u",
                 role_name(ep), (unsigned)msg->type, (unsigned)msg->sequence);
    }
}

static bool data_ready_enabled(const obu_ipc_endpoint_t *ep)
{
    return ep->cfg.gpio_data_ready >= 0;
}

static void set_data_ready(obu_ipc_endpoint_t *ep, int level)
{
    if (data_ready_enabled(ep)) gpio_set_level(ep->cfg.gpio_data_ready, level);
}

static bool slave_peer_fresh(const obu_ipc_endpoint_t *ep, uint64_t now)
{
    return ep->last_valid_rx_us != 0 && now >= ep->last_valid_rx_us &&
           now - ep->last_valid_rx_us <= IPC_SLAVE_PEER_FRESH_US;
}

static bool slave_message_stale(const obu_ipc_message_t *msg, uint64_t now)
{
    return msg->type != OBU_IPC_NOP && msg->source_monotonic_us != 0 &&
           now >= msg->source_monotonic_us && now - msg->source_monotonic_us > IPC_SLAVE_STALE_TX_US;
}

static void master_task(void *arg)
{
    obu_ipc_endpoint_t *ep = arg;
    static uint8_t tx[OBU_IPC_TRANSFER_BYTES];
    static uint8_t rx[OBU_IPC_TRANSFER_BYTES];
    uint32_t nop_sequence = 0;
    uint32_t response_burst_remaining = 0;
    TickType_t last_poll = 0;

    for (;;) {
        const bool outbound = uxQueueMessagesWaiting(ep->txq) > 0;
        const bool ready = data_ready_enabled(ep) && gpio_get_level(ep->cfg.gpio_data_ready) != 0;
        const bool response_burst = response_burst_remaining > 0;
        const TickType_t now_tick = xTaskGetTickCount();
        const bool periodic = (now_tick - last_poll) >= pdMS_TO_TICKS(IPC_POLL_INTERVAL_MS);
        if (!outbound && !ready && !periodic && !response_burst) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        /* Without a DATA_READY wire, a slave response can only be loaded after
         * the transaction that delivered the request has completed. Follow an
         * application request with a short burst of NOP polls so responses such
         * as clock probes are collected in milliseconds instead of 40-60 ms. */
        if (response_burst && !outbound && !ready && !periodic) {
            vTaskDelay(pdMS_TO_TICKS(IPC_RESPONSE_BURST_DELAY_MS));
        }
        last_poll = xTaskGetTickCount();

        obu_ipc_message_t *msg = &ep->tx_scratch;
        prepare_nop(msg, nop_sequence++);
        bool sent_application_message = false;
        if (outbound && xQueueReceive(ep->txq, msg, 0) == pdTRUE) {
            sent_application_message = msg->type != OBU_IPC_NOP;
        }

        memset(tx, 0, sizeof(tx));
        memset(rx, 0, sizeof(rx));
        size_t used = 0;
        const esp_err_t encode_err = obu_ipc_encode(msg, tx, sizeof(tx), &used);
        if (encode_err != ESP_OK) {
            ESP_LOGW(TAG, "S3-master encode failed type=%u: %s", (unsigned)msg->type, esp_err_to_name(encode_err));
            continue;
        }

        spi_transaction_t transaction = {
            .length = OBU_IPC_TRANSFER_BYTES * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        ep->transfer_count++;
        const esp_err_t err = spi_device_transmit(ep->master_dev, &transaction);
        if (err == ESP_OK) {
            enqueue_rx(ep, rx, sizeof(rx));
        } else {
            ep->spi_errors++;
            if (ep->spi_errors == 1u) ESP_LOGE(TAG, "S3-master SPI transaction failed: %s", esp_err_to_name(err));
        }

        if (sent_application_message) {
            response_burst_remaining = IPC_RESPONSE_BURST_POLLS;
        } else if (response_burst_remaining > 0) {
            response_burst_remaining--;
        }

        log_link_stats(ep);
    }
}

static void slave_task(void *arg)
{
    obu_ipc_endpoint_t *ep = arg;
    static uint8_t tx[OBU_IPC_TRANSFER_BYTES];
    static uint8_t rx[OBU_IPC_TRANSFER_BYTES];
    uint32_t nop_sequence = 0;

    for (;;) {
        obu_ipc_message_t *msg = &ep->tx_scratch;
        prepare_nop(msg, nop_sequence++);

        /* Boot order is intentionally irrelevant. Until the master has recently
         * produced a valid frame, keep a NOP transaction armed and leave queued
         * application data untouched. Once the master appears/reappears, discard
         * messages older than the live-link window rather than replaying backlog. */
        const uint64_t now = (uint64_t)esp_timer_get_time();
        bool have_message = false;
        if (slave_peer_fresh(ep, now)) {
            while (xQueueReceive(ep->txq, msg, 0) == pdTRUE) {
                if (!slave_message_stale(msg, now)) {
                    have_message = true;
                    break;
                }
                ep->stale_tx_drops++;
                if (ep->stale_tx_drops == 1u || (ep->stale_tx_drops % 100u) == 0u) {
                    ESP_LOGW(TAG, "C5-slave dropping stale queued message type=%u age=%llu ms stale_tx=%u",
                             (unsigned)msg->type,
                             (unsigned long long)((now - msg->source_monotonic_us) / 1000ULL),
                             (unsigned)ep->stale_tx_drops);
                }
                prepare_nop(msg, nop_sequence++);
            }
        }
        set_data_ready(ep, have_message ? 1 : 0);

        memset(tx, 0, sizeof(tx));
        memset(rx, 0, sizeof(rx));
        size_t used = 0;
        const esp_err_t encode_err = obu_ipc_encode(msg, tx, sizeof(tx), &used);
        if (encode_err != ESP_OK) {
            ESP_LOGW(TAG, "C5-slave encode failed type=%u: %s", (unsigned)msg->type, esp_err_to_name(encode_err));
            set_data_ready(ep, 0);
            continue;
        }

        spi_slave_transaction_t transaction = {
            .length = OBU_IPC_TRANSFER_BYTES * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        ep->transfer_count++;
        const esp_err_t err = spi_slave_transmit(ep->cfg.host, &transaction, portMAX_DELAY);
        set_data_ready(ep, 0);
        if (err == ESP_OK) {
            enqueue_rx(ep, rx, sizeof(rx));
        } else {
            ep->spi_errors++;
            if (ep->spi_errors == 1u) ESP_LOGE(TAG, "C5-slave SPI transaction failed: %s", esp_err_to_name(err));
        }
        log_link_stats(ep);
    }
}

esp_err_t obu_ipc_init(const obu_ipc_config_t *cfg, obu_ipc_endpoint_t **out)
{
    if (cfg == NULL || out == NULL || cfg->queue_depth <= 0) return ESP_ERR_INVALID_ARG;

    obu_ipc_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (ep == NULL) return ESP_ERR_NO_MEM;
    ep->cfg = *cfg;
    ep->txq = xQueueCreate(cfg->queue_depth, sizeof(obu_ipc_message_t));
    ep->rxq = xQueueCreate(cfg->queue_depth, sizeof(obu_ipc_message_t));
    if (ep->txq == NULL || ep->rxq == NULL) {
        if (ep->txq != NULL) vQueueDelete(ep->txq);
        if (ep->rxq != NULL) vQueueDelete(ep->rxq);
        free(ep);
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "init %s: host=%d SCLK=%d MOSI=%d MISO=%d CS=%d READY=%d clock=%dHz transfer=%u queue=%d",
             cfg->role == OBU_IPC_ROLE_S3_MASTER ? "S3-master" : "C5-slave",
             (int)cfg->host, cfg->gpio_sclk, cfg->gpio_mosi, cfg->gpio_miso, cfg->gpio_cs,
             cfg->gpio_data_ready, cfg->clock_hz, (unsigned)OBU_IPC_TRANSFER_BYTES, cfg->queue_depth);

    esp_err_t err;
    if (cfg->role == OBU_IPC_ROLE_S3_MASTER) {
        if (!cfg->bus_already_initialized) {
            spi_bus_config_t bus_config = {
                .mosi_io_num = cfg->gpio_mosi,
                .miso_io_num = cfg->gpio_miso,
                .sclk_io_num = cfg->gpio_sclk,
                .quadwp_io_num = -1,
                .quadhd_io_num = -1,
                .max_transfer_sz = OBU_IPC_TRANSFER_BYTES,
            };
            err = spi_bus_initialize(cfg->host, &bus_config, SPI_DMA_CH_AUTO);
            if (err != ESP_OK) goto fail;
        }

        spi_device_interface_config_t device_config = {
            .clock_speed_hz = cfg->clock_hz,
            .mode = 0,
            .spics_io_num = cfg->gpio_cs,
            .queue_size = 1,
        };
        err = spi_bus_add_device(cfg->host, &device_config, &ep->master_dev);
        if (err != ESP_OK) goto fail;

        if (data_ready_enabled(ep)) {
            gpio_config_t gpio_cfg = {
                .pin_bit_mask = 1ULL << cfg->gpio_data_ready,
                .mode = GPIO_MODE_INPUT,
                .pull_down_en = GPIO_PULLDOWN_ENABLE,
            };
            err = gpio_config(&gpio_cfg);
            if (err != ESP_OK) goto fail;
        }

        if (xTaskCreate(master_task, "obu_ipc_m", IPC_TASK_STACK_BYTES, ep, 12, &ep->task) != pdPASS) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
    } else {
        spi_bus_config_t bus_config = {
            .mosi_io_num = cfg->gpio_mosi,
            .miso_io_num = cfg->gpio_miso,
            .sclk_io_num = cfg->gpio_sclk,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = OBU_IPC_TRANSFER_BYTES,
        };
        spi_slave_interface_config_t slave_config = {
            .mode = 0,
            .spics_io_num = cfg->gpio_cs,
            .queue_size = 1,
        };
        err = spi_slave_initialize(cfg->host, &bus_config, &slave_config, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) goto fail;

        if (data_ready_enabled(ep)) {
            gpio_config_t gpio_cfg = {
                .pin_bit_mask = 1ULL << cfg->gpio_data_ready,
                .mode = GPIO_MODE_OUTPUT,
            };
            err = gpio_config(&gpio_cfg);
            if (err != ESP_OK) goto fail;
            gpio_set_level(cfg->gpio_data_ready, 0);
        }

        if (xTaskCreate(slave_task, "obu_ipc_s", IPC_TASK_STACK_BYTES, ep, 12, &ep->task) != pdPASS) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
    }

    *out = ep;
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "IPC init failed for %s: %s", role_name(ep), esp_err_to_name(err));
    vQueueDelete(ep->txq);
    vQueueDelete(ep->rxq);
    free(ep);
    return err;
}

esp_err_t obu_ipc_send(obu_ipc_endpoint_t *ep, const obu_ipc_message_t *msg, TickType_t timeout)
{
    if (ep == NULL || msg == NULL) return ESP_ERR_INVALID_ARG;
    if (xQueueSend(ep->txq, msg, timeout) == pdTRUE) return ESP_OK;
    ep->tx_queue_drops++;
    ESP_LOGW(TAG, "%s TX queue full; drop type=%u seq=%u", role_name(ep), (unsigned)msg->type, (unsigned)msg->sequence);
    return ESP_ERR_TIMEOUT;
}

esp_err_t obu_ipc_receive(obu_ipc_endpoint_t *ep, obu_ipc_message_t *msg, TickType_t timeout)
{
    if (ep == NULL || msg == NULL) return ESP_ERR_INVALID_ARG;
    return xQueueReceive(ep->rxq, msg, timeout) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

uint32_t obu_ipc_rx_crc_errors(const obu_ipc_endpoint_t *ep)
{
    return ep != NULL ? ep->crc_errors : 0;
}

uint32_t obu_ipc_rx_queue_drops(const obu_ipc_endpoint_t *ep)
{
    return ep != NULL ? ep->queue_drops : 0;
}

uint32_t obu_ipc_tx_queue_drops(const obu_ipc_endpoint_t *ep)
{
    return ep != NULL ? ep->tx_queue_drops : 0;
}
