#pragma once
#include <stddef.h>
#include <stdint.h>
#include "driver/spi_common.h"
#include "esp_err.h"
#include "obu_core.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct obu_diag_logger obu_diag_logger_t;
typedef struct {spi_host_device_t host;int cs_gpio;const char *mount_point;size_t rotate_bytes;uint8_t retain_files;bool bus_already_initialized;} obu_log_config_t;
esp_err_t obu_diag_logger_start(const obu_log_config_t *cfg,obu_diag_logger_t **out);
esp_err_t obu_diag_log_event(obu_diag_logger_t *l,const obu_event_t *event,const char *message);
uint32_t obu_diag_log_drops(const obu_diag_logger_t *l);
#ifdef __cplusplus
}
#endif
