#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "obu_core.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {esp_err_t(*start)(void*);esp_err_t(*send)(void*,const obu_can_frame_t*);esp_err_t(*set_bitrate)(void*,uint32_t);void*ctx;} obu_can_driver_t;
typedef struct {esp_err_t(*start)(void*);esp_err_t(*stop)(void*);esp_err_t(*configure)(void*,const void*cfg,size_t len);void*ctx;} obu_source_driver_t;
typedef struct {esp_err_t(*start_peripheral)(void*);esp_err_t(*notify_event)(void*,const obu_event_t*);bool(*encrypted)(void*);esp_err_t(*reply_control)(void*,uint32_t request_id,esp_err_t result,const void*payload,size_t len);void*ctx;} obu_ble_phone_backend_t;
typedef struct {esp_err_t(*encode_vam)(void*,const void *vam_model,uint8_t*out,size_t cap,size_t*out_len);void*ctx;} obu_vam_codec_t;
typedef struct {bool(*credentials_valid)(void*);esp_err_t(*sign)(void*,uint32_t its_aid,const uint8_t*pdu,size_t len,uint8_t*out,size_t cap,size_t*out_len);void*ctx;} obu_security_backend_t;
typedef struct {bool(*position_valid)(void*);bool(*accuracy_gate_met)(void*);esp_err_t(*reference_position)(void*,void*out_position);void*ctx;} obu_poti_backend_t;
typedef struct {bool(*dcc_allows_tx)(void*);esp_err_t(*wrap_btp_gn)(void*,uint16_t btp_port,const uint8_t*facility,size_t facility_len,uint8_t*out,size_t cap,size_t*out_len);void*ctx;} obu_v2x_transport_backend_t;
typedef struct {esp_err_t(*put)(void*,const obu_event_t*);esp_err_t(*query)(void*,uint32_t kind,void*out,size_t cap,size_t*out_len);void*ctx;} obu_ldm_backend_t;
typedef struct {obu_vam_codec_t *codec;obu_security_backend_t *security;obu_poti_backend_t *poti;obu_v2x_transport_backend_t *transport;bool clustering_disabled;bool vru_profile2;bool vru_stationary_baseline;} obu_vbs_backends_t;
static inline bool obu_vbs_conformance_ready(const obu_vbs_backends_t*b){return b&&b->codec&&b->security&&b->poti&&b->transport&&b->security->credentials_valid&&b->security->credentials_valid(b->security->ctx)&&b->poti->accuracy_gate_met&&b->poti->accuracy_gate_met(b->poti->ctx)&&b->clustering_disabled&&b->vru_profile2&&b->vru_stationary_baseline;}
#ifdef __cplusplus
}
#endif
