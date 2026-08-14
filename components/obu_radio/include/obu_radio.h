#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "obu_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*obu_radio_rx_cb_t)(const obu_v2x_rx_meta_t *meta,const uint8_t *frame,size_t len,void *ctx);
typedef struct {uint16_t frequency_mhz;obu_radio_rx_cb_t rx_cb;void *rx_ctx;} obu_radio_config_t;

typedef struct obu_radio obu_radio_t;
esp_err_t obu_radio_create(const obu_radio_config_t *cfg,obu_radio_t **out);
esp_err_t obu_radio_start(obu_radio_t *r);
esp_err_t obu_radio_set_frequency(obu_radio_t *r,uint16_t mhz);
uint32_t obu_radio_boot_nonce(const obu_radio_t *r);
esp_err_t obu_radio_arm_tx(obu_radio_t *r,uint32_t boot_nonce,bool arm);
esp_err_t obu_radio_transmit(obu_radio_t *r,const uint8_t *frame,size_t len);
void obu_radio_get_status(obu_radio_t *r,obu_radio_status_t *out);

#ifdef __cplusplus
}
#endif
