#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBU_IPC_MAGIC 0x3142554fu
#define OBU_IPC_VERSION 1u
#define OBU_IPC_PAYLOAD_MAX 2464u
#define OBU_IPC_TRANSFER_BYTES 2528u

typedef enum {
    OBU_IPC_NOP=0, OBU_IPC_RX_FRAME=1, OBU_IPC_RADIO_STATUS=2, OBU_IPC_TX_REQUEST=3,
    OBU_IPC_TX_RESULT=4, OBU_IPC_SET_RADIO=5, OBU_IPC_ARM_TX=6,
    OBU_IPC_TIME_PROBE=7, OBU_IPC_TIME_RESPONSE=8, OBU_IPC_ACK=9
} obu_ipc_type_t;

typedef struct {
    obu_ipc_type_t type;
    uint32_t sequence;
    uint32_t flags;
    uint64_t source_monotonic_us;
    uint16_t payload_len;
    uint8_t payload[OBU_IPC_PAYLOAD_MAX];
} obu_ipc_message_t;

typedef enum { OBU_IPC_ROLE_S3_MASTER=0, OBU_IPC_ROLE_C5_SLAVE=1 } obu_ipc_role_t;

typedef struct {
    obu_ipc_role_t role;
    spi_host_device_t host;
    int gpio_sclk;
    int gpio_mosi;
    int gpio_miso;
    int gpio_cs;
    int gpio_data_ready;
    int queue_depth;
    int clock_hz;
    bool bus_already_initialized;
} obu_ipc_config_t;

typedef struct obu_ipc_endpoint obu_ipc_endpoint_t;

esp_err_t obu_ipc_encode(const obu_ipc_message_t *msg, uint8_t *out, size_t out_cap, size_t *out_len);
esp_err_t obu_ipc_decode(const uint8_t *buf, size_t len, obu_ipc_message_t *msg);
esp_err_t obu_ipc_init(const obu_ipc_config_t *cfg, obu_ipc_endpoint_t **out);
esp_err_t obu_ipc_send(obu_ipc_endpoint_t *ep, const obu_ipc_message_t *msg, TickType_t timeout);
esp_err_t obu_ipc_receive(obu_ipc_endpoint_t *ep, obu_ipc_message_t *msg, TickType_t timeout);
uint32_t obu_ipc_rx_crc_errors(const obu_ipc_endpoint_t *ep);
uint32_t obu_ipc_rx_queue_drops(const obu_ipc_endpoint_t *ep);
uint32_t obu_ipc_tx_queue_drops(const obu_ipc_endpoint_t *ep);

#ifdef __cplusplus
}
#endif
