#include "sdkconfig.h"
#include "obu_radio.h"
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#ifndef CONFIG_IDF_TARGET_ESP32C5
#error "obu_radio_esp32c5 is only valid for ESP32-C5"
#endif

#define RADIO_MAX_FRAME 2400u
#define RADIO_POOL 12u
static const char*TAG="obu_radio";
void phy_change_channel(int channel,int arg1,int arg2,int ht_mode);
void phy_11p_set(int enable,int arg);

typedef struct {obu_v2x_rx_meta_t meta;uint16_t len;uint8_t data[RADIO_MAX_FRAME];} slot_t;
struct obu_radio {obu_radio_config_t cfg;slot_t slots[RADIO_POOL];QueueHandle_t freeq,readyq;TaskHandle_t task;bool started,armed;uint32_t nonce,seq;obu_radio_status_t st;portMUX_TYPE tslock;uint32_t last_ts32;uint64_t ts_high;};
static obu_radio_t *g_radio;

static uint64_t extend_ts(obu_radio_t*r,uint32_t v){portENTER_CRITICAL(&r->tslock);if(v<r->last_ts32 && r->last_ts32-v>0x80000000u)r->ts_high+=1ULL<<32;r->last_ts32=v;uint64_t x=r->ts_high|v;portEXIT_CRITICAL(&r->tslock);return x;}
static void rx_cb(void *buf,wifi_promiscuous_pkt_type_t type){
    obu_radio_t*r=g_radio;if(!r||type==WIFI_PKT_MISC)return;wifi_promiscuous_pkt_t*p=buf;if(p->rx_ctrl.rx_state)return;uint16_t n;
#if CONFIG_SOC_WIFI_HE_SUPPORT
    n=(uint16_t)p->rx_ctrl.dump_len;
#else
    n=(uint16_t)p->rx_ctrl.sig_len;if(n>4)n-=4;
#endif
    uint8_t idx;if(xQueueReceive(r->freeq,&idx,0)!=pdTRUE){r->st.rx_drop_no_buffer++;return;}if(n>RADIO_MAX_FRAME){r->st.rx_drop_oversize++;xQueueSend(r->freeq,&idx,0);return;}
    slot_t*s=&r->slots[idx];s->len=n;s->meta.frequency_mhz=r->cfg.frequency_mhz;s->meta.rssi_dbm=p->rx_ctrl.rssi;s->meta.wifi_packet_type=(uint8_t)type;s->meta.rx_state=p->rx_ctrl.rx_state;s->meta.original_len=n;s->meta.captured_len=n;s->meta.c5_sequence=r->seq++;s->meta.radio_hw_timestamp_us=extend_ts(r,p->rx_ctrl.timestamp);s->meta.c5_rx_monotonic_us=obu_monotonic_us();memcpy(s->data,p->payload,n);
    if(xQueueSend(r->readyq,&idx,0)!=pdTRUE){r->st.rx_drop_no_buffer++;xQueueSend(r->freeq,&idx,0);}else r->st.rx_frames++;
}
static void worker(void*arg){obu_radio_t*r=arg;for(;;){uint8_t i;if(xQueueReceive(r->readyq,&i,portMAX_DELAY)==pdTRUE){slot_t*s=&r->slots[i];if(r->cfg.rx_cb)r->cfg.rx_cb(&s->meta,s->data,s->len,r->cfg.rx_ctx);xQueueSend(r->freeq,&i,portMAX_DELAY);}}}

esp_err_t obu_radio_create(const obu_radio_config_t*c,obu_radio_t**out){if(!c||!out)return ESP_ERR_INVALID_ARG;if(c->frequency_mhz<5860||c->frequency_mhz>5900||c->frequency_mhz%10)return ESP_ERR_INVALID_ARG;obu_radio_t*r=calloc(1,sizeof(*r));if(!r)return ESP_ERR_NO_MEM;r->cfg=*c;r->nonce=esp_random();if(!r->nonce)r->nonce=1;r->tslock=(portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;r->freeq=xQueueCreate(RADIO_POOL,sizeof(uint8_t));r->readyq=xQueueCreate(RADIO_POOL,sizeof(uint8_t));for(uint8_t i=0;i<RADIO_POOL;i++)xQueueSend(r->freeq,&i,0);r->st.frequency_mhz=c->frequency_mhz;r->st.boot_nonce=r->nonce;r->st.reset_reason=(uint32_t)esp_reset_reason();const esp_app_desc_t*d=esp_app_get_description();if(d)strncpy(r->st.firmware_version,d->version,sizeof(r->st.firmware_version)-1);*out=r;return ESP_OK;}

esp_err_t obu_radio_start(obu_radio_t*r){if(!r)return ESP_ERR_INVALID_ARG;if(r->started)return ESP_OK;esp_err_t e=nvs_flash_init();if(e==ESP_ERR_NVS_NO_FREE_PAGES||e==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());e=nvs_flash_init();}if(e!=ESP_OK)return e;esp_err_t le=esp_event_loop_create_default();if(le!=ESP_OK&&le!=ESP_ERR_INVALID_STATE)return le;wifi_init_config_t init=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&init));ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_start());wifi_promiscuous_filter_t f={.filter_mask=WIFI_PROMIS_FILTER_MASK_ALL & ~WIFI_PROMIS_FILTER_MASK_FCSFAIL};esp_wifi_set_promiscuous_filter(&f);g_radio=r;esp_wifi_set_promiscuous_rx_cb(rx_cb);ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));phy_11p_set(1,0);ESP_ERROR_CHECK(esp_wifi_set_channel(140,WIFI_SECOND_CHAN_NONE));phy_change_channel(r->cfg.frequency_mhz,1,0,0);r->armed=false;r->started=true;r->st.radio_running=true;r->st.tx_armed=false;xTaskCreate(worker,"v2x_rx",8192,r,15,&r->task);ESP_LOGW(TAG,"802.11p uses ESP32-C5 private ROM hooks; RF/HIL qualification required");return ESP_OK;}
esp_err_t obu_radio_set_frequency(obu_radio_t*r,uint16_t mhz){if(!r||mhz<5860||mhz>5900||mhz%10)return ESP_ERR_INVALID_ARG;r->cfg.frequency_mhz=mhz;r->st.frequency_mhz=mhz;if(r->started)phy_change_channel(mhz,1,0,0);return ESP_OK;}
uint32_t obu_radio_boot_nonce(const obu_radio_t*r){return r?r->nonce:0;}
esp_err_t obu_radio_arm_tx(obu_radio_t*r,uint32_t nonce,bool arm){if(!r||nonce!=r->nonce)return ESP_ERR_INVALID_ARG;r->armed=arm;r->st.tx_armed=arm;return ESP_OK;}
esp_err_t obu_radio_transmit(obu_radio_t*r,const uint8_t*frame,size_t len){if(!r||!frame||len<24||len>RADIO_MAX_FRAME)return ESP_ERR_INVALID_ARG;r->st.tx_requests++;if(!r->started||!r->armed){r->st.tx_failed++;return ESP_ERR_INVALID_STATE;}esp_err_t e=esp_wifi_80211_tx(WIFI_IF_STA,frame,(int)len,false);if(e==ESP_OK)r->st.tx_success++;else r->st.tx_failed++;return e;}
void obu_radio_get_status(obu_radio_t*r,obu_radio_status_t*out){if(r&&out){r->st.tx_armed=r->armed;r->st.boot_nonce=r->nonce;*out=r->st;}}
