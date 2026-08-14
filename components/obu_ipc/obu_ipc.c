#include "obu_ipc.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define HDR_LEN 32u
#define IPC_POLL_INTERVAL_MS 20u

struct obu_ipc_endpoint {
    obu_ipc_config_t cfg;
    QueueHandle_t txq;
    QueueHandle_t rxq;
    TaskHandle_t task;
    spi_device_handle_t master_dev;
    uint32_t crc_errors;
    uint32_t queue_drops;
    uint32_t tx_queue_drops;
};

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

static uint32_t crc32_ieee(const uint8_t *data, size_t len)
{
    uint32_t crc = ~0u;
    while (len-- > 0) {
        crc ^= *data++;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

esp_err_t obu_ipc_encode(const obu_ipc_message_t *msg,
                         uint8_t *out,
                         size_t out_cap,
                         size_t *out_len)
{
    if (msg == NULL || out == NULL || msg->payload_len > OBU_IPC_PAYLOAD_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t encoded_len = HDR_LEN + msg->payload_len;
    if (out_cap < encoded_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    memset(out, 0, encoded_len);
    wr32(out, OBU_IPC_MAGIC);
    out[4] = OBU_IPC_VERSION;
    out[5] = (uint8_t)msg->type;
    wr16(out + 6, HDR_LEN);
    wr16(out + 8, msg->payload_len);
    wr32(out + 12, msg->sequence);
    wr32(out + 16, msg->flags);
    wr64(out + 20, msg->source_monotonic_us);
    if (msg->payload_len > 0) {
        memcpy(out + HDR_LEN, msg->payload, msg->payload_len);
    }

    wr32(out + 28, 0);
    wr32(out + 28, crc32_ieee(out, encoded_len));
    if (out_len != NULL) {
        *out_len = encoded_len;
    }
    return ESP_OK;
}

esp_err_t obu_ipc_decode(const uint8_t *buf, size_t len, obu_ipc_message_t *msg)
{
    if (buf == NULL || msg == NULL || len < HDR_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (rd32(buf) != OBU_IPC_MAGIC ||
        buf[4] != OBU_IPC_VERSION ||
        rd16(buf + 6) != HDR_LEN) {
        return ESP_ERR_INVALID_VERSION;
    }

    const uint16_t payload_len = rd16(buf + 8);
    const size_t encoded_len = HDR_LEN + payload_len;
    if (payload_len > OBU_IPC_PAYLOAD_MAX || encoded_len > len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t tmp[OBU_IPC_TRANSFER_BYTES];
    memcpy(tmp, buf, encoded_len);
    const uint32_t expected_crc = rd32(tmp + 28);
    wr32(tmp + 28, 0);
    if (crc32_ieee(tmp, encoded_len) != expected_crc) {
        return ESP_ERR_INVALID_CRC;
    }

    memset(msg, 0, sizeof(*msg));
    msg->type = (obu_ipc_type_t)buf[5];
    msg->payload_len = payload_len;
    msg->sequence = rd32(buf + 12);
    msg->flags = rd32(buf + 16);
    msg->source_monotonic_us = rd64(buf + 20);
    if (payload_len > 0) {
        memcpy(msg->payload, buf + HDR_LEN, payload_len);
    }
    return ESP_OK;
}

static void enqueue_rx(obu_ipc_endpoint_t *ep, const uint8_t *buf, size_t len)
{
    obu_ipc_message_t msg;
    const esp_err_t err = obu_ipc_decode(buf, len, &msg);
    if (err == ESP_ERR_INVALID_CRC) {
        ep->crc_errors++;
        return;
    }
    if (err != ESP_OK || msg.type == OBU_IPC_NOP) {
        return;
    }
    if (xQueueSend(ep->rxq, &msg, 0) != pdTRUE) {
        ep->queue_drops++;
    }
}

static bool data_ready_enabled(const obu_ipc_endpoint_t *ep)
{
    return ep->cfg.gpio_data_ready >= 0;
}

static void set_data_ready(obu_ipc_endpoint_t *ep, int level)
{
    if (data_ready_enabled(ep)) {
        gpio_set_level(ep->cfg.gpio_data_ready, level);
    }
}

static void master_task(void *arg)
{
    obu_ipc_endpoint_t *ep = arg;
    static uint8_t tx[OBU_IPC_TRANSFER_BYTES];
    static uint8_t rx[OBU_IPC_TRANSFER_BYTES];
    uint32_t nop_sequence = 0;
    TickType_t last_poll = 0;

    for (;;) {
        const bool outbound = uxQueueMessagesWaiting(ep->txq) > 0;
        const bool ready = data_ready_enabled(ep) && gpio_get_level(ep->cfg.gpio_data_ready) != 0;
        const TickType_t now_tick = xTaskGetTickCount();
        const bool periodic = (now_tick - last_poll) >= pdMS_TO_TICKS(IPC_POLL_INTERVAL_MS);
        if (!outbound && !ready && !periodic) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        last_poll = now_tick;

        obu_ipc_message_t msg = {
            .type = OBU_IPC_NOP,
            .sequence = nop_sequence++,
            .source_monotonic_us = (uint64_t)esp_timer_get_time(),
        };
        if (outbound) {
            (void)xQueueReceive(ep->txq, &msg, 0);
        }

        memset(tx, 0, sizeof(tx));
        memset(rx, 0, sizeof(rx));
        size_t used = 0;
        if (obu_ipc_encode(&msg, tx, sizeof(tx), &used) != ESP_OK) {
            continue;
        }

        spi_transaction_t transaction = {
            .length = OBU_IPC_TRANSFER_BYTES * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        if (spi_device_transmit(ep->master_dev, &transaction) == ESP_OK) {
            enqueue_rx(ep, rx, sizeof(rx));
        }
    }
}

static void slave_task(void *arg)
{
    obu_ipc_endpoint_t *ep = arg;
    static uint8_t tx[OBU_IPC_TRANSFER_BYTES];
    static uint8_t rx[OBU_IPC_TRANSFER_BYTES];
    uint32_t nop_sequence = 0;

    for (;;) {
        obu_ipc_message_t msg = {
            .type = OBU_IPC_NOP,
            .sequence = nop_sequence++,
            .source_monotonic_us = (uint64_t)esp_timer_get_time(),
        };
        const bool have_message = xQueueReceive(ep->txq, &msg, 0) == pdTRUE;
        set_data_ready(ep, have_message ? 1 : 0);

        memset(tx, 0, sizeof(tx));
        memset(rx, 0, sizeof(rx));
        size_t used = 0;
        if (obu_ipc_encode(&msg, tx, sizeof(tx), &used) != ESP_OK) {
            set_data_ready(ep, 0);
            continue;
        }

        spi_slave_transaction_t transaction = {
            .length = OBU_IPC_TRANSFER_BYTES * 8,
            .tx_buffer = tx,
            .rx_buffer = rx,
        };
        const esp_err_t err = spi_slave_transmit(ep->cfg.host, &transaction, portMAX_DELAY);
        set_data_ready(ep, 0);
        if (err == ESP_OK) {
            enqueue_rx(ep, rx, sizeof(rx));
        }
    }
}

esp_err_t obu_ipc_init(const obu_ipc_config_t *cfg, obu_ipc_endpoint_t **out)
{
    if (cfg == NULL || out == NULL || cfg->queue_depth <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    obu_ipc_endpoint_t *ep = calloc(1, sizeof(*ep));
    if (ep == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ep->cfg = *cfg;
    ep->txq = xQueueCreate(cfg->queue_depth, sizeof(obu_ipc_message_t));
    ep->rxq = xQueueCreate(cfg->queue_depth, sizeof(obu_ipc_message_t));
    if (ep->txq == NULL || ep->rxq == NULL) {
        if (ep->txq != NULL) {
            vQueueDelete(ep->txq);
        }
        if (ep->rxq != NULL) {
            vQueueDelete(ep->rxq);
        }
        free(ep);
        return ESP_ERR_NO_MEM;
    }

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
            if (err != ESP_OK) {
                goto fail;
            }
        }

        spi_device_interface_config_t device_config = {
            .clock_speed_hz = cfg->clock_hz,
            .mode = 0,
            .spics_io_num = cfg->gpio_cs,
            .queue_size = 1,
        };
        err = spi_bus_add_device(cfg->host, &device_config, &ep->master_dev);
        if (err != ESP_OK) {
            goto fail;
        }

        if (data_ready_enabled(ep)) {
            gpio_config_t gpio_cfg = {
                .pin_bit_mask = 1ULL << cfg->gpio_data_ready,
                .mode = GPIO_MODE_INPUT,
                .pull_down_en = GPIO_PULLDOWN_ENABLE,
            };
            err = gpio_config(&gpio_cfg);
            if (err != ESP_OK) {
                goto fail;
            }
        }

        if (xTaskCreate(master_task, "obu_ipc_m", 8192, ep, 12, &ep->task) != pdPASS) {
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
        if (err != ESP_OK) {
            goto fail;
        }

        if (data_ready_enabled(ep)) {
            gpio_config_t gpio_cfg = {
                .pin_bit_mask = 1ULL << cfg->gpio_data_ready,
                .mode = GPIO_MODE_OUTPUT,
            };
            err = gpio_config(&gpio_cfg);
            if (err != ESP_OK) {
                goto fail;
            }
            gpio_set_level(cfg->gpio_data_ready, 0);
        }

        if (xTaskCreate(slave_task, "obu_ipc_s", 8192, ep, 12, &ep->task) != pdPASS) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
    }

    *out = ep;
    return ESP_OK;

fail:
    vQueueDelete(ep->txq);
    vQueueDelete(ep->rxq);
    free(ep);
    return err;
}

esp_err_t obu_ipc_send(obu_ipc_endpoint_t *ep,
                       const obu_ipc_message_t *msg,
                       TickType_t timeout)
{
    if (ep == NULL || msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xQueueSend(ep->txq, msg, timeout) == pdTRUE) {
        return ESP_OK;
    }
    ep->tx_queue_drops++;
    return ESP_ERR_TIMEOUT;
}

esp_err_t obu_ipc_receive(obu_ipc_endpoint_t *ep,
                          obu_ipc_message_t *msg,
                          TickType_t timeout)
{
    if (ep == NULL || msg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
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
