#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "driver/gpio.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {OBU_HMI_WARN_NONE=0,OBU_HMI_WARN_DENM,OBU_HMI_WARN_SYSTEM} obu_hmi_warning_t;
typedef struct {float speed_kmh;bool speed_valid;bool phone_connected;bool c5_online;bool gnss_valid;bool glosa_valid;float glosa_target_kmh;obu_hmi_warning_t warning;char warning_text[24];} obu_hmi_model_t;
typedef struct obu_display_driver obu_display_driver_t;
typedef struct {esp_err_t(*render)(obu_display_driver_t*,const obu_hmi_model_t*);esp_err_t(*set_enabled)(obu_display_driver_t*,bool);} obu_display_ops_t;
struct obu_display_driver {const obu_display_ops_t *ops;void *ctx;};
esp_err_t obu_ssd1306_create(i2c_master_bus_handle_t bus,uint8_t addr,obu_display_driver_t *out);
esp_err_t obu_buzzer_init(gpio_num_t gpio);
esp_err_t obu_buzzer_beep(uint32_t frequency_hz,uint32_t duration_ms);
#ifdef __cplusplus
}
#endif
