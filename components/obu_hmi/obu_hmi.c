#include "obu_hmi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct{i2c_master_dev_handle_t dev;bool enabled;uint8_t fb[1024];}ssd_t;
static const uint8_t font[][5]={
[' '-' ']={0,0,0,0,0},['.'-' ']={0,0x60,0x60,0,0},['0'-' ']={0x3e,0x51,0x49,0x45,0x3e},['1'-' ']={0,0x42,0x7f,0x40,0},['2'-' ']={0x42,0x61,0x51,0x49,0x46},['3'-' ']={0x21,0x41,0x45,0x4b,0x31},['4'-' ']={0x18,0x14,0x12,0x7f,0x10},['5'-' ']={0x27,0x45,0x45,0x45,0x39},['6'-' ']={0x3c,0x4a,0x49,0x49,0x30},['7'-' ']={0x01,0x71,0x09,0x05,0x03},['8'-' ']={0x36,0x49,0x49,0x49,0x36},['9'-' ']={0x06,0x49,0x49,0x29,0x1e},
['A'-' ']={0x7e,0x11,0x11,0x11,0x7e},['B'-' ']={0x7f,0x49,0x49,0x49,0x36},['C'-' ']={0x3e,0x41,0x41,0x41,0x22},['D'-' ']={0x7f,0x41,0x41,0x22,0x1c},['E'-' ']={0x7f,0x49,0x49,0x49,0x41},['F'-' ']={0x7f,0x09,0x09,0x09,0x01},['G'-' ']={0x3e,0x41,0x49,0x49,0x7a},['H'-' ']={0x7f,0x08,0x08,0x08,0x7f},['I'-' ']={0,0x41,0x7f,0x41,0},['J'-' ']={0x20,0x40,0x41,0x3f,0x01},['K'-' ']={0x7f,0x08,0x14,0x22,0x41},['L'-' ']={0x7f,0x40,0x40,0x40,0x40},['M'-' ']={0x7f,0x02,0x0c,0x02,0x7f},['N'-' ']={0x7f,0x04,0x08,0x10,0x7f},['O'-' ']={0x3e,0x41,0x41,0x41,0x3e},['P'-' ']={0x7f,0x09,0x09,0x09,0x06},['Q'-' ']={0x3e,0x41,0x51,0x21,0x5e},['R'-' ']={0x7f,0x09,0x19,0x29,0x46},['S'-' ']={0x46,0x49,0x49,0x49,0x31},['T'-' ']={0x01,0x01,0x7f,0x01,0x01},['U'-' ']={0x3f,0x40,0x40,0x40,0x3f},['V'-' ']={0x1f,0x20,0x40,0x20,0x1f},['W'-' ']={0x3f,0x40,0x38,0x40,0x3f},['X'-' ']={0x63,0x14,0x08,0x14,0x63},['Y'-' ']={0x07,0x08,0x70,0x08,0x07},['Z'-' ']={0x61,0x51,0x49,0x45,0x43}
};

static esp_err_t cmd(ssd_t*s,uint8_t c){uint8_t b[2]={0,c};return i2c_master_transmit(s->dev,b,2,50);}
static void text(ssd_t*s,int x,int page,const char*str){while(*str&&x<122){char c=*str++;if(c>='a'&&c<='z')c-=32;if(c<' '||c>'Z')c=' ';const uint8_t*g=font[c-' '];for(int i=0;i<5;i++)s->fb[page*128+x++]=g[i];s->fb[page*128+x++]=0;}}
static esp_err_t flush(ssd_t*s){for(int p=0;p<8;p++){cmd(s,0xb0|p);cmd(s,0x00);cmd(s,0x10);uint8_t b[129];b[0]=0x40;memcpy(b+1,s->fb+p*128,128);esp_err_t e=i2c_master_transmit(s->dev,b,sizeof(b),100);if(e!=ESP_OK)return e;}return ESP_OK;}

static const char *v2x_signal_quality(int rssi_dbm)
{
    if (rssi_dbm >= -65) return "STRONG";
    if (rssi_dbm >= -80) return "GOOD";
    if (rssi_dbm >= -95) return "WEAK";
    return "POOR";
}

static const char *lora_signal_quality(float rssi_dbm)
{
    if (rssi_dbm >= -85.0f) return "STRONG";
    if (rssi_dbm >= -105.0f) return "GOOD";
    if (rssi_dbm >= -120.0f) return "WEAK";
    return "POOR";
}

static void format_link_age(char *out, size_t out_len, uint32_t age_ms)
{
    if (age_ms == 0xffffffffU) {
        snprintf(out, out_len, "NO DATA");
    } else if (age_ms < 1000U) {
        snprintf(out, out_len, "%luMS", (unsigned long)age_ms);
    } else if (age_ms < 10000U) {
        snprintf(out, out_len, "%.1FS", (double)age_ms / 1000.0);
    } else {
        snprintf(out, out_len, "%luS", (unsigned long)(age_ms / 1000U));
    }
}

static esp_err_t render(obu_display_driver_t*d,const obu_hmi_model_t*m)
{
    ssd_t*s=d->ctx;
    if(!s->enabled)return ESP_OK;
    memset(s->fb,0,sizeof(s->fb));
    char b[32];
    char age[12];
    format_link_age(age,sizeof(age),m->c5_last_age_ms);

    /* C5 controller and 5.9 GHz radio */
    if(m->c5_message_count==0){
        snprintf(b,sizeof(b),"C5 WAITING FOR LINK");
    }else if(m->c5_last_age_ms>3000U){
        snprintf(b,sizeof(b),"C5 STALE %s",age);
    }else if(m->c5_online){
        snprintf(b,sizeof(b),"C5 OK RADIO ON %s",age);
    }else{
        snprintf(b,sizeof(b),"C5 OK RADIO OFF");
    }
    text(s,0,0,b);

    /* GNSS */
    snprintf(b,sizeof(b),"GNSS %s",m->gnss_valid?"OK FIX":"SEARCHING");
    text(s,0,1,b);

    /* V2X receive path */
    if(!m->c5_online){
        snprintf(b,sizeof(b),"V2X OFF RADIO OFF");
    }else if(m->v2x_rx_count==0&&!m->v2x_rx_seen){
        snprintf(b,sizeof(b),"V2X READY NO RX");
    }else{
        const char*type=m->v2x_rx_type[0]?m->v2x_rx_type:"RAW";
        snprintf(b,sizeof(b),"V2X OK %.6s RX%lu",type,(unsigned long)m->v2x_rx_count);
    }
    text(s,0,2,b);

    if(m->v2x_rx_count>0||m->v2x_rx_seen){
        snprintf(b,sizeof(b),"V2X RF %s %dDBM",
                 v2x_signal_quality((int)m->v2x_last_rssi_dbm),
                 (int)m->v2x_last_rssi_dbm);
    }else{
        snprintf(b,sizeof(b),"V2X RF NO SIGNAL YET");
    }
    text(s,0,3,b);

    /* Optional LoRaWAN uplink */
    if(!m->lorawan_enabled){
        snprintf(b,sizeof(b),"LORA DISABLED");
    }else if(!m->lorawan_ready){
        snprintf(b,sizeof(b),"LORA START ERROR");
    }else if(!m->lorawan_joined&&m->lorawan_join_failures>0){
        snprintf(b,sizeof(b),"LORA RETRYING JOIN");
    }else if(!m->lorawan_joined){
        snprintf(b,sizeof(b),"LORA JOINING");
    }else{
        snprintf(b,sizeof(b),"LORA OK JOINED");
    }
    text(s,0,4,b);

    if(m->lorawan_joined&&m->lorawan_signal_valid){
        snprintf(b,sizeof(b),"LORA RF %s %.0FDBM",
                 lora_signal_quality(m->lorawan_last_rssi_dbm),
                 (double)m->lorawan_last_rssi_dbm);
    }else if(m->lorawan_joined){
        snprintf(b,sizeof(b),"LORA RF NO DOWNLINK");
    }else{
        snprintf(b,sizeof(b),"LORA RF WAITING");
    }
    text(s,0,5,b);

    if(!m->lorawan_enabled){
        snprintf(b,sizeof(b),"UPLINK DISABLED");
    }else if(!m->lorawan_joined){
        snprintf(b,sizeof(b),"UPLINK WAITING");
    }else if(m->lorawan_tx_errors>0){
        snprintf(b,sizeof(b),"UPLINK ERROR E%lu T%lu",
                 (unsigned long)m->lorawan_tx_errors,
                 (unsigned long)m->lorawan_frames_sent);
    }else{
        snprintf(b,sizeof(b),"UPLINK OK TX%lu",
                 (unsigned long)m->lorawan_frames_sent);
    }
    text(s,0,6,b);

    /* Highest-priority user-facing state occupies the last row. */
    if(m->warning!=OBU_HMI_WARN_NONE){
        snprintf(b,sizeof(b),"ALERT %.14s",m->warning_text[0]?m->warning_text:"WARNING");
    }else if(m->glosa_valid){
        snprintf(b,sizeof(b),"PHONE %s GLOSA ON",m->phone_connected?"OK":"OFF");
    }else{
        snprintf(b,sizeof(b),"PHONE %s GLOSA OFF",m->phone_connected?"OK":"OFF");
    }
    text(s,0,7,b);

    return flush(s);
}

static esp_err_t enable(obu_display_driver_t*d,bool on){ssd_t*s=d->ctx;s->enabled=on;return cmd(s,on?0xaf:0xae);}
static const obu_display_ops_t ops={render,enable};

esp_err_t obu_ssd1306_create(i2c_master_bus_handle_t bus,uint8_t addr,obu_display_driver_t*out){if(!bus||!out)return ESP_ERR_INVALID_ARG;ssd_t*s=calloc(1,sizeof(*s));if(!s)return ESP_ERR_NO_MEM;i2c_device_config_t c={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=addr,.scl_speed_hz=400000};ESP_ERROR_CHECK(i2c_master_bus_add_device(bus,&c,&s->dev));uint8_t init[]={0xae,0xd5,0x80,0xa8,0x3f,0xd3,0x00,0x40,0x8d,0x14,0x20,0x02,0xa1,0xc8,0xda,0x12,0x81,0xcf,0xd9,0xf1,0xdb,0x40,0xa4,0xa6,0xaf};for(size_t i=0;i<sizeof(init);i++)ESP_ERROR_CHECK(cmd(s,init[i]));s->enabled=true;out->ops=&ops;out->ctx=s;return ESP_OK;}

static gpio_num_t buzzer_gpio=GPIO_NUM_NC;
esp_err_t obu_buzzer_init(gpio_num_t gpio){buzzer_gpio=gpio;ledc_timer_config_t t={.speed_mode=LEDC_LOW_SPEED_MODE,.duty_resolution=LEDC_TIMER_10_BIT,.timer_num=LEDC_TIMER_0,.freq_hz=2000,.clk_cfg=LEDC_AUTO_CLK};esp_err_t e=ledc_timer_config(&t);if(e!=ESP_OK)return e;ledc_channel_config_t c={.gpio_num=gpio,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,.intr_type=LEDC_INTR_DISABLE,.timer_sel=LEDC_TIMER_0,.duty=0,.hpoint=0};return ledc_channel_config(&c);}
esp_err_t obu_buzzer_beep(uint32_t hz,uint32_t ms){if(buzzer_gpio==GPIO_NUM_NC||hz<100||hz>10000)return ESP_ERR_INVALID_STATE;ESP_ERROR_CHECK(ledc_set_freq(LEDC_LOW_SPEED_MODE,LEDC_TIMER_0,hz));ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,512));ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));vTaskDelay(pdMS_TO_TICKS(ms));ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,0));return ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0);}
