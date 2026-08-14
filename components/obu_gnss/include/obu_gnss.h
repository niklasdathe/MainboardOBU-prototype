#pragma once
#include <stdint.h>
#include "driver/uart.h"
#include "esp_err.h"
#include "obu_core.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {uart_port_t uart;int tx_gpio;int rx_gpio;int baud;obu_bus_t *bus;} obu_l76k_config_t;
typedef struct obu_l76k obu_l76k_t;
esp_err_t obu_l76k_start(const obu_l76k_config_t *cfg,obu_l76k_t **out);
#ifdef __cplusplus
}
#endif
