#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "obu_core.h"
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {int64_t utc_ns;uint64_t mono_us;obu_time_quality_t quality;bool valid;} obu_time_anchor_t;
typedef struct {i2c_master_dev_handle_t rtc;obu_time_anchor_t anchor;uint64_t last_pps_us;portMUX_TYPE lock;} obu_time_service_t;
esp_err_t obu_time_init(obu_time_service_t *t,i2c_master_bus_handle_t bus);
esp_err_t obu_time_read_rtc(obu_time_service_t *t,int64_t *utc_ns);
esp_err_t obu_time_set_rtc(obu_time_service_t *t,int64_t utc_ns);
void obu_time_ingest_pps(obu_time_service_t *t,uint64_t mono_us);
esp_err_t obu_time_ingest_gnss_utc(obu_time_service_t *t,int64_t utc_ns,uint64_t rx_mono_us,bool use_recent_pps);
bool obu_time_now(obu_time_service_t *t,uint64_t mono_us,int64_t *utc_ns,obu_time_quality_t *quality);
#ifdef __cplusplus
}
#endif
