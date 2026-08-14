#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/spi_master.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "obu_core.h"
#include "obu_gnss.h"
#include "obu_hmi.h"
#include "obu_ifaces.h"
#include "obu_ipc.h"
#include "obu_log.h"
#include "obu_otm.h"
#include "obu_time.h"
#include "obu_warning.h"

static const char *TAG = "s3_main";
static obu_bus_t bus;
static obu_ipc_endpoint_t *ipc;
static obu_display_driver_t display;
static obu_time_service_t timesvc;
static obu_diag_logger_t *logger;
static obu_otm_t *otm;
static obu_hmi_model_t hmi;
static obu_warning_output_t buzzer_output;
static obu_warning_controller_t warning_controller;
static bool warning_output_ready;
static uint32_t seq;
static uint32_t ipc_seq;
static QueueHandle_t pps_queue;
static QueueHandle_t event_queue;

typedef struct __attribute__((packed)) {
    obu_v2x_rx_meta_t meta;
    uint16_t frame_len;
} rx_wire_t;

typedef struct {
    int64_t c5_to_s3_offset_us;
    uint32_t samples;
    uint32_t last_rtt_us;
    bool valid;
    portMUX_TYPE lock;
} c5_clock_relation_t;

static c5_clock_relation_t c5_clock = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static uint64_t c5_to_s3_monotonic(uint64_t c5_us)
{
    portENTER_CRITICAL(&c5_clock.lock);
    int64_t offset = c5_clock.c5_to_s3_offset_us;
    bool valid = c5_clock.valid;
    portEXIT_CRITICAL(&c5_clock.lock);
    if (!valid) return obu_monotonic_us();
    int64_t mapped = (int64_t)c5_us + offset;
    return mapped > 0 ? (uint64_t)mapped : 0;
}

static void update_clock_relation(uint64_t t1_s3, uint64_t t2_c5, uint64_t t3_c5, uint64_t t4_s3)
{
    if (t4_s3 < t1_s3 || t3_c5 < t2_c5) return;
    const uint64_t rtt = (t4_s3 - t1_s3) - (t3_c5 - t2_c5);
    if (rtt > 20000) return;
    const int64_t sample = ((int64_t)t1_s3 - (int64_t)t2_c5 +
                            (int64_t)t4_s3 - (int64_t)t3_c5) / 2;
    portENTER_CRITICAL(&c5_clock.lock);
    if (!c5_clock.valid) {
        c5_clock.c5_to_s3_offset_us = sample;
        c5_clock.samples = 1;
        c5_clock.valid = true;
    } else {
        c5_clock.c5_to_s3_offset_us = (c5_clock.c5_to_s3_offset_us * 7 + sample) / 8;
        c5_clock.samples++;
    }
    c5_clock.last_rtt_us = (uint32_t)rtt;
    portEXIT_CRITICAL(&c5_clock.lock);
}

static void stamp_event_utc(obu_event_t *e)
{
    uint64_t mono = e->hub_monotonic_us ? e->hub_monotonic_us : obu_monotonic_us();
    (void)obu_time_now(&timesvc, mono, &e->utc_ns, &e->time_quality);
}

static void publish_clock_status(void)
{
    obu_clock_sync_status_t st;
    portENTER_CRITICAL(&c5_clock.lock);
    st = (obu_clock_sync_status_t){
        .c5_to_s3_offset_us = c5_clock.c5_to_s3_offset_us,
        .last_rtt_us = c5_clock.last_rtt_us,
        .samples = c5_clock.samples,
        .valid = c5_clock.valid,
    };
    portEXIT_CRITICAL(&c5_clock.lock);
    obu_event_t e = {
        .source = OBU_SOURCE_SYSTEM,
        .type = OBU_DATA_CLOCK_SYNC,
        .sequence = seq++,
        .flags = st.valid ? OBU_EVENT_F_VALID : 0,
        .source_monotonic_us = obu_monotonic_us(),
        .payload_len = sizeof(st),
    };
    e.hub_monotonic_us = e.source_monotonic_us;
    memcpy(e.payload, &st, sizeof(st));
    stamp_event_utc(&e);
    (void)obu_bus_publish(&bus, &e);
}

static void ingest_ipc(const obu_ipc_message_t *m)
{
    if (m->type == OBU_IPC_RX_FRAME && m->payload_len >= sizeof(rx_wire_t)) {
        rx_wire_t w;
        memcpy(&w, m->payload, sizeof(w));
        if (sizeof(w) + w.frame_len > m->payload_len) return;
        obu_event_t e = {
            .source = OBU_SOURCE_C5_RADIO,
            .type = OBU_DATA_V2X_RAW_RX,
            .sequence = seq++,
            .flags = OBU_EVENT_F_RAW | OBU_EVENT_F_VALID,
            .source_monotonic_us = w.meta.c5_rx_monotonic_us,
            .hub_monotonic_us = c5_to_s3_monotonic(w.meta.c5_rx_monotonic_us),
            .validity_ms = 0,
        };
        size_t total = sizeof(w.meta) + w.frame_len;
        if (total > OBU_EVENT_PAYLOAD_MAX) return;
        e.payload_len = (uint16_t)total;
        memcpy(e.payload, &w.meta, sizeof(w.meta));
        memcpy(e.payload + sizeof(w.meta), m->payload + sizeof(w), w.frame_len);
        stamp_event_utc(&e);
        (void)obu_bus_publish(&bus, &e);
        if (otm) (void)obu_otm_publish_live_frame(otm, m->payload + sizeof(w), w.frame_len);
    } else if (m->type == OBU_IPC_RADIO_STATUS && m->payload_len >= sizeof(obu_radio_status_t)) {
        obu_radio_status_t s;
        memcpy(&s, m->payload, sizeof(s));
        hmi.c5_online = s.radio_running;
        obu_event_t e = {
            .source = OBU_SOURCE_C5_RADIO,
            .type = OBU_DATA_RADIO_STATUS,
            .sequence = seq++,
            .flags = OBU_EVENT_F_VALID,
            .source_monotonic_us = m->source_monotonic_us,
            .hub_monotonic_us = c5_to_s3_monotonic(m->source_monotonic_us),
            .payload_len = sizeof(s),
        };
        memcpy(e.payload, &s, sizeof(s));
        stamp_event_utc(&e);
        (void)obu_bus_publish(&bus, &e);
    } else if (m->type == OBU_IPC_TX_RESULT) {
        obu_event_t e = {
            .source = OBU_SOURCE_C5_RADIO,
            .type = OBU_DATA_V2X_TX_RESULT,
            .sequence = seq++,
            .flags = OBU_EVENT_F_VALID,
            .source_monotonic_us = m->source_monotonic_us,
            .hub_monotonic_us = c5_to_s3_monotonic(m->source_monotonic_us),
            .payload_len = m->payload_len,
        };
        memcpy(e.payload, m->payload, m->payload_len);
        stamp_event_utc(&e);
        (void)obu_bus_publish(&bus, &e);
    } else if (m->type == OBU_IPC_TIME_RESPONSE && m->payload_len >= 24) {
        uint64_t t1, t2, t3;
        memcpy(&t1, m->payload, 8);
        memcpy(&t2, m->payload + 8, 8);
        memcpy(&t3, m->payload + 16, 8);
        update_clock_relation(t1, t2, t3, obu_monotonic_us());
        publish_clock_status();
    }
}

static void process_gnss(const obu_event_t *e)
{
    if (e->type != OBU_DATA_GNSS_FIX || e->payload_len < sizeof(obu_gnss_fix_t)) return;
    obu_gnss_fix_t f;
    memcpy(&f, e->payload, sizeof(f));
    hmi.gnss_valid = f.position_valid;
    if (f.velocity_valid) {
        hmi.speed_valid = true;
        hmi.speed_kmh = (float)f.speed_mm_s * 0.0036f;
    }
    if (f.utc_valid) {
        (void)obu_time_ingest_gnss_utc(&timesvc, f.utc_ns,
                                       e->hub_monotonic_us ? e->hub_monotonic_us : e->source_monotonic_us,
                                       true);
    }
}

static void process_warning(const obu_event_t *e)
{
    obu_warning_notification_t warning = {
        .notification_id = 0,
        .kind = OBU_WARNING_KIND_DENM,
        .severity = OBU_WARNING_SEVERITY_WARNING,
        .active = true,
        .audible = true,
    };
    snprintf(warning.text, sizeof(warning.text), "V2X WARNING");

    if (e->payload_len >= sizeof(warning)) {
        memcpy(&warning, e->payload, sizeof(warning));
    }

    hmi.warning = warning.kind == OBU_WARNING_KIND_SYSTEM ? OBU_HMI_WARN_SYSTEM : OBU_HMI_WARN_DENM;
    snprintf(hmi.warning_text, sizeof(hmi.warning_text), "%s",
             warning.text[0] ? warning.text : "V2X WARNING");

    if (warning_output_ready) {
        esp_err_t err = obu_warning_controller_handle(&warning_controller, &warning);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "warning output failed: %s", esp_err_to_name(err));
        }
    }
}

static void event_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        obu_event_t e;
        if (xQueueReceive(event_queue, &e, portMAX_DELAY) != pdTRUE) continue;
        process_gnss(&e);
        if (!e.utc_ns) stamp_event_utc(&e);
        if (logger && (e.type == OBU_DATA_DIAGNOSTIC || e.type == OBU_DATA_RADIO_STATUS ||
                       e.type == OBU_DATA_V2X_TX_RESULT || e.type == OBU_DATA_CLOCK_SYNC || !(e.flags & OBU_EVENT_F_VALID))) {
            (void)obu_diag_log_event(logger, &e, "event");
        }
        if (e.type == OBU_DATA_WARNING) {
            process_warning(&e);
        }
    }
}

static void hmi_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (display.ops != NULL && display.ops->render != NULL) {
            (void)display.ops->render(&display, &hmi);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void time_probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        obu_ipc_message_t m = {
            .type = OBU_IPC_TIME_PROBE,
            .sequence = ipc_seq++,
            .source_monotonic_us = obu_monotonic_us(),
            .payload_len = 8,
        };
        memcpy(m.payload, &m.source_monotonic_us, 8);
        (void)obu_ipc_send(ipc, &m, pdMS_TO_TICKS(5));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    uint64_t now = (uint64_t)esp_timer_get_time();
    BaseType_t hp = pdFALSE;
    (void)xQueueSendFromISR(pps_queue, &now, &hp);
    if (hp) portYIELD_FROM_ISR();
}

static void pps_task(void *arg)
{
    (void)arg;
    uint64_t ts;
    for (;;) {
        if (xQueueReceive(pps_queue, &ts, portMAX_DELAY) == pdTRUE) obu_time_ingest_pps(&timesvc, ts);
    }
}

static esp_err_t start_pps(void)
{
    pps_queue = xQueueCreate(4, sizeof(uint64_t));
    if (!pps_queue) return ESP_ERR_NO_MEM;
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << GPIO_NUM_41,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gc), TAG, "PPS gpio");
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(GPIO_NUM_41, pps_isr, NULL), TAG, "PPS isr");
    if (xTaskCreate(pps_task, "gnss_pps", 3072, NULL, 11, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void publish_startup_status(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char text[128];
    snprintf(text, sizeof(text), "S3 boot reset=%u fw=%s", (unsigned)esp_reset_reason(), d ? d->version : "unknown");
    obu_event_t e = {
        .source = OBU_SOURCE_SYSTEM,
        .type = OBU_DATA_DIAGNOSTIC,
        .sequence = seq++,
        .flags = OBU_EVENT_F_VALID,
        .source_monotonic_us = obu_monotonic_us(),
    };
    e.hub_monotonic_us = e.source_monotonic_us;
    e.payload_len = (uint16_t)strnlen(text, sizeof(text));
    memcpy(e.payload, text, e.payload_len);
    stamp_event_utc(&e);
    (void)obu_bus_publish(&bus, &e);
}

void app_main(void)
{
    obu_bus_init(&bus);

    i2c_master_bus_config_t ib = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = 5,
        .scl_io_num = 6,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c;
    ESP_ERROR_CHECK(i2c_new_master_bus(&ib, &i2c));
    ESP_ERROR_CHECK(obu_time_init(&timesvc, i2c));

    esp_err_t display_err = obu_ssd1306_create(i2c, 0x3c, &display);
    if (display_err != ESP_OK) {
        ESP_LOGW(TAG, "OLED unavailable; acquisition continues: %s", esp_err_to_name(display_err));
    }
    hmi.phone_connected = false;

#ifdef CONFIG_OBU_BUZZER_ENABLE
    obu_expansion_buzzer_config_t buzzer_config = {
        .gpio = GPIO_NUM_4,
        .frequency_hz = CONFIG_OBU_BUZZER_FREQUENCY_HZ,
        .duration_ms = CONFIG_OBU_BUZZER_DURATION_MS,
        .queue_depth = 4,
    };
    esp_err_t buzzer_err = obu_expansion_buzzer_create(&buzzer_config, &buzzer_output);
    if (buzzer_err == ESP_OK) {
        obu_warning_controller_init(&warning_controller, &buzzer_output);
#ifdef CONFIG_OBU_BUZZER_DEFAULT_MUTED
        (void)obu_warning_controller_set_audio_enabled(&warning_controller, false);
#endif
        warning_output_ready = true;
    } else {
        ESP_LOGW(TAG, "local warning buzzer unavailable; acquisition continues: %s",
                 esp_err_to_name(buzzer_err));
    }
#endif

    spi_bus_config_t sb = {
        .mosi_io_num = 9,
        .miso_io_num = 8,
        .sclk_io_num = 7,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OBU_IPC_TRANSFER_BYTES,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &sb, SPI_DMA_CH_AUTO));
    obu_ipc_config_t ic = {
        .role = OBU_IPC_ROLE_S3_MASTER,
        .host = SPI2_HOST,
        .gpio_sclk = 7,
        .gpio_miso = 8,
        .gpio_mosi = 9,
        .gpio_cs = 1,
        .gpio_data_ready = -1,
        .queue_depth = 12,
        .clock_hz = 8000000,
        .bus_already_initialized = true,
    };
    ESP_ERROR_CHECK(obu_ipc_init(&ic, &ipc));

    event_queue = xQueueCreate(8, sizeof(obu_event_t));
    if (!event_queue) abort();
    uint8_t event_subscriber_id;
    ESP_ERROR_CHECK(obu_bus_subscribe(&bus, event_queue, &event_subscriber_id));
    if (xTaskCreate(event_consumer, "event_consumer", 7168, NULL, 9, NULL) != pdPASS) abort();

    obu_l76k_t *gnss = NULL;
    obu_l76k_config_t gc = {
        .uart = UART_NUM_1,
        .tx_gpio = 43,
        .rx_gpio = 44,
        .baud = 9600,
        .bus = &bus,
    };
    if (obu_l76k_start(&gc, &gnss) != ESP_OK) ESP_LOGE(TAG, "GNSS start failed");
    if (start_pps() != ESP_OK) ESP_LOGW(TAG, "GNSS PPS unavailable; UTC falls back to sentence timing/RTC");

    obu_log_config_t lc = {
        .host = SPI2_HOST,
        .cs_gpio = 3,
        .mount_point = "/sd",
        .rotate_bytes = 256 * 1024,
        .retain_files = 8,
        .bus_already_initialized = true,
    };
    if (obu_diag_logger_start(&lc, &logger) != ESP_OK) ESP_LOGW(TAG, "SD diagnostic log unavailable");

#ifdef CONFIG_OBU_OTM_DIRECT_ENABLE
    const bool otm_direct_enabled = true;
#else
    const bool otm_direct_enabled = false;
#endif
    obu_otm_wifi_config_t oc = {
        .enabled = otm_direct_enabled,
        .wifi_ssid = CONFIG_OBU_WIFI_SSID,
        .wifi_password = CONFIG_OBU_WIFI_PASSWORD,
        .broker_uri = "mqtts://cits1.opentrafficmap.org:8883",
        .node_id = CONFIG_OBU_OTM_NODE_ID,
    };
    if (obu_otm_wifi_start(&oc, &otm) != ESP_OK) ESP_LOGW(TAG, "OTM uploader not started");

    publish_startup_status();
    if (display.ops != NULL && xTaskCreate(hmi_task, "hmi", 4096, NULL, 5, NULL) != pdPASS) abort();
    if (xTaskCreate(time_probe_task, "c5_clock", 3072, NULL, 8, NULL) != pdPASS) abort();

    for (;;) {
        obu_ipc_message_t m;
        if (obu_ipc_receive(ipc, &m, pdMS_TO_TICKS(250)) == ESP_OK) ingest_ipc(&m);
    }
}
