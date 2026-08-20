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
typedef struct {
    float speed_kmh;
    bool speed_valid;
    bool phone_connected;
    bool c5_online;
    bool gnss_valid;
    bool glosa_valid;
    float glosa_target_kmh;
    bool v2x_rx_seen;
    char v2x_rx_type[16];
    uint32_t v2x_rx_count;
    int16_t v2x_last_rssi_dbm;
    uint16_t v2x_last_frequency_mhz;
    bool lorawan_enabled;
    bool lorawan_ready;
    bool lorawan_joined;
    bool lorawan_signal_valid;
    float lorawan_last_rssi_dbm;
    float lorawan_last_snr_db;
    uint32_t lorawan_frames_sent;
    uint32_t lorawan_tx_errors;
    uint32_t lorawan_join_attempts;
    uint32_t lorawan_join_failures;
    uint32_t c5_message_count;
    uint32_t c5_last_age_ms;
    obu_hmi_warning_t warning;
    char warning_text[48];
} obu_hmi_model_t;
typedef struct obu_display_driver obu_display_driver_t;
typedef struct {esp_err_t(*render)(obu_display_driver_t*,const obu_hmi_model_t*);esp_err_t(*set_enabled)(obu_display_driver_t*,bool);} obu_display_ops_t;
struct obu_display_driver {const obu_display_ops_t *ops;void *ctx;};
esp_err_t obu_ssd1306_create(i2c_master_bus_handle_t bus,uint8_t addr,obu_display_driver_t *out);
esp_err_t obu_buzzer_init(gpio_num_t gpio);
esp_err_t obu_buzzer_beep(uint32_t frequency_hz,uint32_t duration_ms);
#ifdef __cplusplus
}
#endif
