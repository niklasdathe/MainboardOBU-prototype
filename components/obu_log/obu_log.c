#include "obu_log.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
struct obu_diag_logger{obu_log_config_t c;FILE*f;size_t bytes;uint32_t drops;unsigned index;};
static void rotate(struct obu_diag_logger*l){if(l->f){fflush(l->f);fclose(l->f);l->f=NULL;}l->index=(l->index+1)%l->c.retain_files;char p[96];snprintf(p,sizeof(p),"%s/diag_%02u.log",l->c.mount_point,l->index);l->f=fopen(p,"w");l->bytes=0;}
esp_err_t obu_diag_logger_start(const obu_log_config_t*c,obu_diag_logger_t**out){if(!c||!out||!c->mount_point||!c->retain_files)return ESP_ERR_INVALID_ARG;struct obu_diag_logger*l=calloc(1,sizeof(*l));if(!l)return ESP_ERR_NO_MEM;l->c=*c;sdmmc_host_t host=SDSPI_HOST_DEFAULT();host.slot=c->host;sdspi_device_config_t slot=SDSPI_DEVICE_CONFIG_DEFAULT();slot.gpio_cs=c->cs_gpio;slot.host_id=c->host;esp_vfs_fat_sdmmc_mount_config_t m={.format_if_mount_failed=false,.max_files=4,.allocation_unit_size=16*1024};sdmmc_card_t*card=NULL;esp_err_t e=esp_vfs_fat_sdspi_mount(c->mount_point,&host,&slot,&m,&card);if(e!=ESP_OK){free(l);return e;}rotate(l);if(!l->f){free(l);return ESP_FAIL;}*out=l;return ESP_OK;}
esp_err_t obu_diag_log_event(obu_diag_logger_t*l,const obu_event_t*e,const char*msg){if(!l||!l->f)return ESP_ERR_INVALID_STATE;if(l->bytes>=l->c.rotate_bytes)rotate(l);int n=fprintf(l->f,"%llu,%lld,%u,%u,%u,%u,%s\n",(unsigned long long)(e?e->source_monotonic_us:obu_monotonic_us()),(long long)(e?e->utc_ns:0),(unsigned)(e?e->time_quality:0),(unsigned)(e?e->source:OBU_SOURCE_SYSTEM),(unsigned)(e?e->type:OBU_DATA_DIAGNOSTIC),(unsigned)(e?e->flags:0),msg?msg:"");if(n<0){l->drops++;return ESP_FAIL;}l->bytes+=(size_t)n;if(e&&(e->type==OBU_DATA_DIAGNOSTIC||!(e->flags&OBU_EVENT_F_VALID)))fflush(l->f);return ESP_OK;}
uint32_t obu_diag_log_drops(const obu_diag_logger_t*l){return l?l->drops:0;}
