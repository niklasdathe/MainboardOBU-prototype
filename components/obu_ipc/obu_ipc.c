#include "obu_ipc.h"
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/spi_slave.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define HDR_LEN 32u
static const char *TAG="obu_ipc";

struct obu_ipc_endpoint {
    obu_ipc_config_t cfg;
    QueueHandle_t txq, rxq;
    TaskHandle_t task;
    spi_device_handle_t master_dev;
    uint32_t crc_errors, queue_drops, tx_queue_drops;
};

static uint16_t rd16(const uint8_t*p){return (uint16_t)p[0]|((uint16_t)p[1]<<8);}
static uint32_t rd32(const uint8_t*p){return (uint32_t)rd16(p)|((uint32_t)rd16(p+2)<<16);}
static uint64_t rd64(const uint8_t*p){return (uint64_t)rd32(p)|((uint64_t)rd32(p+4)<<32);}
static void wr16(uint8_t*p,uint16_t v){p[0]=v;p[1]=v>>8;}
static void wr32(uint8_t*p,uint32_t v){wr16(p,v);wr16(p+2,v>>16);}
static void wr64(uint8_t*p,uint64_t v){wr32(p,v);wr32(p+4,v>>32);}
static uint32_t crc32_ieee(const uint8_t *p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&-(int32_t)(c&1));}return ~c;}

esp_err_t obu_ipc_encode(const obu_ipc_message_t *m,uint8_t*out,size_t cap,size_t*out_len){
    if(!m||!out||m->payload_len>OBU_IPC_PAYLOAD_MAX) return ESP_ERR_INVALID_ARG;
    size_t n=HDR_LEN+m->payload_len; if(cap<n) return ESP_ERR_INVALID_SIZE;
    memset(out,0,n); wr32(out,OBU_IPC_MAGIC); out[4]=OBU_IPC_VERSION; out[5]=(uint8_t)m->type;
    wr16(out+6,HDR_LEN); wr16(out+8,m->payload_len); wr32(out+12,m->sequence); wr32(out+16,m->flags);
    wr64(out+20,m->source_monotonic_us); if(m->payload_len) memcpy(out+HDR_LEN,m->payload,m->payload_len);
    wr32(out+28,0); wr32(out+28,crc32_ieee(out,n)); if(out_len)*out_len=n; return ESP_OK;
}

esp_err_t obu_ipc_decode(const uint8_t*buf,size_t len,obu_ipc_message_t*m){
    if(!buf||!m||len<HDR_LEN) return ESP_ERR_INVALID_SIZE;
    if(rd32(buf)!=OBU_IPC_MAGIC||buf[4]!=OBU_IPC_VERSION||rd16(buf+6)!=HDR_LEN) return ESP_ERR_INVALID_VERSION;
    uint16_t plen=rd16(buf+8); if(plen>OBU_IPC_PAYLOAD_MAX||HDR_LEN+plen>len) return ESP_ERR_INVALID_SIZE;
    uint8_t tmp[OBU_IPC_TRANSFER_BYTES]; size_t n=HDR_LEN+plen; memcpy(tmp,buf,n); uint32_t want=rd32(tmp+28); wr32(tmp+28,0);
    if(crc32_ieee(tmp,n)!=want) return ESP_ERR_INVALID_CRC;
    memset(m,0,sizeof(*m));m->type=(obu_ipc_type_t)buf[5];m->payload_len=plen;m->sequence=rd32(buf+12);m->flags=rd32(buf+16);m->source_monotonic_us=rd64(buf+20);
    if(plen)memcpy(m->payload,buf+HDR_LEN,plen);return ESP_OK;
}

static void enqueue_rx(obu_ipc_endpoint_t *ep,const uint8_t*buf,size_t len){
    obu_ipc_message_t m; esp_err_t e=obu_ipc_decode(buf,len,&m); if(e==ESP_ERR_INVALID_CRC){ep->crc_errors++;return;} if(e!=ESP_OK||m.type==OBU_IPC_NOP)return;
    if(xQueueSend(ep->rxq,&m,0)!=pdTRUE)ep->queue_drops++;
}

static void master_task(void*arg){
    obu_ipc_endpoint_t*ep=arg; static uint8_t tx[OBU_IPC_TRANSFER_BYTES],rx[OBU_IPC_TRANSFER_BYTES]; uint32_t nopseq=0;
    TickType_t last_poll=0;
    for(;;){
        bool outbound=uxQueueMessagesWaiting(ep->txq)>0; bool ready=gpio_get_level(ep->cfg.gpio_data_ready);
        TickType_t now_tick=xTaskGetTickCount();
        bool periodic=(now_tick-last_poll)>=pdMS_TO_TICKS(20);
        if(!outbound&&!ready&&!periodic){vTaskDelay(pdMS_TO_TICKS(2));continue;}
        last_poll=now_tick;
        obu_ipc_message_t m={.type=OBU_IPC_NOP,.sequence=nopseq++,.source_monotonic_us=(uint64_t)esp_timer_get_time()};
        if(outbound)xQueueReceive(ep->txq,&m,0); memset(tx,0,sizeof(tx));memset(rx,0,sizeof(rx));size_t used=0;obu_ipc_encode(&m,tx,sizeof(tx),&used);
        spi_transaction_t t={.length=OBU_IPC_TRANSFER_BYTES*8,.tx_buffer=tx,.rx_buffer=rx};
        if(spi_device_transmit(ep->master_dev,&t)==ESP_OK) enqueue_rx(ep,rx,sizeof(rx));
    }
}

static void slave_task(void*arg){
    obu_ipc_endpoint_t*ep=arg; static uint8_t tx[OBU_IPC_TRANSFER_BYTES],rx[OBU_IPC_TRANSFER_BYTES]; uint32_t nopseq=0;
    for(;;){
        obu_ipc_message_t m={.type=OBU_IPC_NOP,.sequence=nopseq++,.source_monotonic_us=(uint64_t)esp_timer_get_time()};
        bool have=xQueueReceive(ep->txq,&m,0)==pdTRUE; gpio_set_level(ep->cfg.gpio_data_ready,have?1:0);
        memset(tx,0,sizeof(tx));memset(rx,0,sizeof(rx));size_t used=0;obu_ipc_encode(&m,tx,sizeof(tx),&used);
        spi_slave_transaction_t t={.length=OBU_IPC_TRANSFER_BYTES*8,.tx_buffer=tx,.rx_buffer=rx};
        esp_err_t e=spi_slave_transmit(ep->cfg.host,&t,portMAX_DELAY);gpio_set_level(ep->cfg.gpio_data_ready,0);
        if(e==ESP_OK)enqueue_rx(ep,rx,sizeof(rx));
    }
}

esp_err_t obu_ipc_init(const obu_ipc_config_t*c,obu_ipc_endpoint_t**out){
    if(!c||!out||c->queue_depth<=0)return ESP_ERR_INVALID_ARG;obu_ipc_endpoint_t*ep=calloc(1,sizeof(*ep));if(!ep)return ESP_ERR_NO_MEM;ep->cfg=*c;
    ep->txq=xQueueCreate(c->queue_depth,sizeof(obu_ipc_message_t));ep->rxq=xQueueCreate(c->queue_depth,sizeof(obu_ipc_message_t));if(!ep->txq||!ep->rxq)return ESP_ERR_NO_MEM;
    if(c->role==OBU_IPC_ROLE_S3_MASTER){
        if(!c->bus_already_initialized){spi_bus_config_t b={.mosi_io_num=c->gpio_mosi,.miso_io_num=c->gpio_miso,.sclk_io_num=c->gpio_sclk,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=OBU_IPC_TRANSFER_BYTES};ESP_RETURN_ON_ERROR(spi_bus_initialize(c->host,&b,SPI_DMA_CH_AUTO),TAG,"bus");}
        spi_device_interface_config_t d={.clock_speed_hz=c->clock_hz,.mode=0,.spics_io_num=c->gpio_cs,.queue_size=1};ESP_RETURN_ON_ERROR(spi_bus_add_device(c->host,&d,&ep->master_dev),TAG,"device");
        gpio_config_t g={.pin_bit_mask=1ULL<<c->gpio_data_ready,.mode=GPIO_MODE_INPUT,.pull_down_en=GPIO_PULLDOWN_ENABLE};ESP_RETURN_ON_ERROR(gpio_config(&g),TAG,"ready");
        xTaskCreate(master_task,"obu_ipc_m",8192,ep,12,&ep->task);
    } else {
        spi_bus_config_t b={.mosi_io_num=c->gpio_mosi,.miso_io_num=c->gpio_miso,.sclk_io_num=c->gpio_sclk,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=OBU_IPC_TRANSFER_BYTES};
        spi_slave_interface_config_t s={.mode=0,.spics_io_num=c->gpio_cs,.queue_size=1};ESP_RETURN_ON_ERROR(spi_slave_initialize(c->host,&b,&s,SPI_DMA_CH_AUTO),TAG,"slave");
        gpio_config_t g={.pin_bit_mask=1ULL<<c->gpio_data_ready,.mode=GPIO_MODE_OUTPUT};ESP_RETURN_ON_ERROR(gpio_config(&g),TAG,"ready");gpio_set_level(c->gpio_data_ready,0);
        xTaskCreate(slave_task,"obu_ipc_s",8192,ep,12,&ep->task);
    }
    *out=ep;return ESP_OK;
}

esp_err_t obu_ipc_send(obu_ipc_endpoint_t*ep,const obu_ipc_message_t*m,TickType_t t){if(!ep||!m)return ESP_ERR_INVALID_ARG;if(xQueueSend(ep->txq,m,t)==pdTRUE)return ESP_OK;ep->tx_queue_drops++;return ESP_ERR_TIMEOUT;}
esp_err_t obu_ipc_receive(obu_ipc_endpoint_t*ep,obu_ipc_message_t*m,TickType_t t){if(!ep||!m)return ESP_ERR_INVALID_ARG;return xQueueReceive(ep->rxq,m,t)==pdTRUE?ESP_OK:ESP_ERR_TIMEOUT;}
uint32_t obu_ipc_rx_crc_errors(const obu_ipc_endpoint_t*ep){return ep?ep->crc_errors:0;}
uint32_t obu_ipc_rx_queue_drops(const obu_ipc_endpoint_t*ep){return ep?ep->queue_drops:0;}
uint32_t obu_ipc_tx_queue_drops(const obu_ipc_endpoint_t*ep){return ep?ep->tx_queue_drops:0;}
