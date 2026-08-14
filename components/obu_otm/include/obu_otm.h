#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {esp_err_t(*start)(void*ctx);bool(*ready)(void*ctx);esp_err_t(*publish)(void*ctx,const char*topic,const void*data,size_t len,bool retain);void(*stop)(void*ctx);void*ctx;} obu_uplink_t;
typedef struct {bool enabled;const char*wifi_ssid;const char*wifi_password;const char*broker_uri;const char*node_id;} obu_otm_wifi_config_t;
typedef struct {uint32_t attempted;uint32_t successful;uint32_t failed;uint32_t dropped_disconnected;bool connected;} obu_otm_stats_t;
typedef struct obu_otm obu_otm_t;
esp_err_t obu_otm_wifi_start(const obu_otm_wifi_config_t *cfg,obu_otm_t **out);
esp_err_t obu_otm_publish_live_frame(obu_otm_t *o,const uint8_t *frame,size_t len);
uint32_t obu_otm_drop_disconnected(const obu_otm_t *o);
uint32_t obu_otm_publish_errors(const obu_otm_t *o);
void obu_otm_get_stats(const obu_otm_t *o,obu_otm_stats_t *stats);
#ifdef __cplusplus
}
#endif
