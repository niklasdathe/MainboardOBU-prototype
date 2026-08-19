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
#include "obu_lorawan.h"
#include "obu_time.h"
#include "obu_v2x.h"
#include "obu_warning.h"

static const char *TAG = "s3_main";
static obu_bus_t bus;
static obu_ipc_endpoint_t *ipc;
static obu_display_driver_t display;
static obu_time_service_t timesvc;
static obu_diag_logger_t *logger;
static obu_lorawan_t *lorawan;
static obu_hmi_model_t hmi;
static obu_warning_output_t buzzer_output;
static obu_warning_controller_t warning_controller;
static bool warning_output_ready;
static uint32_t seq;
static uint32_t ipc_seq;
static QueueHandle_t pps_queue;
static QueueHandle_t event_queue;

static obu_ipc_message_t main_ipc_rx;
static obu_ipc_message_t time_probe_message;
static obu_event_t main_event_scratch;
static obu_event_t event_consumer_scratch;

static uint64_t c5_last_message_us;
static uint64_t c5_last_link_report_us;
static uint32_t c5_message_count;
static uint32_t c5_status_count;
static uint32_t c5_time_response_count;
static uint32_t c5_rx_frame_count;
static uint32_t c5_last_boot_nonce;
static bool c5_link_online;

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

static const char *ipc_type_name(obu_ipc_type_t type)
{
    switch (type) {
        case OBU_IPC_NOP: return "NOP";
        case OBU_IPC_RX_FRAME: return "RX_FRAME";
        case OBU_IPC_RADIO_STATUS: return "RADIO_STATUS";
        case OBU_IPC_TX_REQUEST: return "TX_REQUEST";
        case OBU_IPC_TX_RESULT: return "TX_RESULT";
        case OBU_IPC_SET_RADIO: return "SET_RADIO";
        case OBU_IPC_ARM_TX: return "ARM_TX";
        case OBU_IPC_TIME_PROBE: return "TIME_PROBE";
        case OBU_IPC_TIME_RESPONSE: return "TIME_RESPONSE";
        case OBU_IPC_ACK: return "ACK";
        default: return "UNKNOWN";
    }
}

static void reset_c5_clock(const char *reason)
{
    bool was_valid;
    uint32_t old_samples;
    portENTER_CRITICAL(&c5_clock.lock);
    was_valid = c5_clock.valid;
    old_samples = c5_clock.samples;
    c5_clock.c5_to_s3_offset_us = 0;
    c5_clock.samples = 0;
    c5_clock.last_rtt_us = 0;
    c5_clock.valid = false;
    portEXIT_CRITICAL(&c5_clock.lock);

    if (was_valid || old_samples != 0) {
        ESP_LOGI(TAG, "C5 clock relation invalidated: %s", reason != NULL ? reason : "link state changed");
    }
}

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
    if (rtt > 20000) {
        ESP_LOGW(TAG, "C5 clock sample rejected: RTT=%llu us exceeds 20 ms", (unsigned long long)rtt);
        return;
    }
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

static obu_event_t *prepare_main_event(void)
{
    memset(&main_event_scratch, 0, sizeof(main_event_scratch));
    return &main_event_scratch;
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

    obu_event_t *e = prepare_main_event();
    e->source = OBU_SOURCE_SYSTEM;
    e->type = OBU_DATA_CLOCK_SYNC;
    e->sequence = seq++;
    e->flags = st.valid ? OBU_EVENT_F_VALID : 0;
    e->source_monotonic_us = obu_monotonic_us();
    e->hub_monotonic_us = e->source_monotonic_us;
    e->payload_len = sizeof(st);
    memcpy(e->payload, &st, sizeof(st));
    stamp_event_utc(e);
    (void)obu_bus_publish(&bus, e);
}

static void mark_c5_message(const obu_ipc_message_t *m)
{
    const uint64_t now = obu_monotonic_us();
    const bool had_previous_link = c5_last_message_us != 0;
    c5_last_message_us = now;
    c5_message_count++;
    if (!c5_link_online) {
        if (had_previous_link) {
            reset_c5_clock("C5 application link resumed after timeout");
            publish_clock_status();
        }
        c5_link_online = true;
        ESP_LOGI(TAG, "C5 application IPC active: first/resumed message type=%s(%u) seq=%u",
                 ipc_type_name(m->type), (unsigned)m->type, (unsigned)m->sequence);
    }
    ESP_LOGD(TAG, "C5 IPC RX type=%s(%u) seq=%u payload=%u",
             ipc_type_name(m->type), (unsigned)m->type, (unsigned)m->sequence, (unsigned)m->payload_len);
}

static void update_v2x_hmi_and_geiger(const uint8_t *frame, size_t frame_len)
{
    hmi.v2x_rx_seen = true;

    obu_v2x_frame_info_t info;
    if (!obu_v2x_classify_80211_frame(frame, frame_len, &info)) {
        snprintf(hmi.v2x_rx_type, sizeof(hmi.v2x_rx_type), "RAW");
        return;
    }

    if (info.kind == OBU_V2X_FRAME_FACILITIES) {
        snprintf(hmi.v2x_rx_type, sizeof(hmi.v2x_rx_type), "%s",
                 obu_v2x_message_type_name(info.message_id));
    } else if (info.kind == OBU_V2X_FRAME_SECURED) {
        snprintf(hmi.v2x_rx_type, sizeof(hmi.v2x_rx_type), "SECURED");
    } else {
        snprintf(hmi.v2x_rx_type, sizeof(hmi.v2x_rx_type), "GN");
    }

#ifdef CONFIG_OBU_GEIGER_COUNTER_ENABLE
    if (warning_output_ready) {
        const esp_err_t beep_err = obu_expansion_buzzer_pulse(
            &buzzer_output,
            CONFIG_OBU_GEIGER_COUNTER_FREQUENCY_HZ,
            CONFIG_OBU_GEIGER_COUNTER_DURATION_MS);
        if (beep_err != ESP_OK && beep_err != ESP_ERR_TIMEOUT) {
            ESP_LOGW(TAG, "V2X Geiger buzzer failed: %s", esp_err_to_name(beep_err));
        }
    }
#endif
}

static void ingest_ipc(const obu_ipc_message_t *m)
{
    mark_c5_message(m);

    if (m->type == OBU_IPC_RX_FRAME && m->payload_len >= sizeof(rx_wire_t)) {
        rx_wire_t w;
        memcpy(&w, m->payload, sizeof(w));
        if (sizeof(w) + w.frame_len > m->payload_len) {
            ESP_LOGW(TAG, "C5 RX_FRAME malformed: frame_len=%u payload_len=%u",
                     (unsigned)w.frame_len, (unsigned)m->payload_len);
            return;
        }

        const uint8_t *frame = m->payload + sizeof(w);
#ifdef CONFIG_OBU_LORAWAN_ENABLE
        if (lorawan != NULL) {
            const esp_err_t uplink_err = obu_lorawan_enqueue_frame(lorawan, frame, w.frame_len);
            if (uplink_err != ESP_OK && uplink_err != ESP_ERR_TIMEOUT &&
                uplink_err != ESP_ERR_INVALID_SIZE && uplink_err != ESP_ERR_NOT_SUPPORTED) {
                ESP_LOGD(TAG, "LoRaWAN enqueue failed: %s", esp_err_to_name(uplink_err));
            }
        }
#endif

        size_t total = sizeof(w.meta) + w.frame_len;
        if (total > OBU_EVENT_PAYLOAD_MAX) {
            ESP_LOGW(TAG, "C5 RX_FRAME too large for event bus: %u bytes", (unsigned)total);
            return;
        }

        update_v2x_hmi_and_geiger(frame, w.frame_len);

        obu_event_t *e = prepare_main_event();
        e->source = OBU_SOURCE_V2X;
        e->type = OBU_DATA_V2X_RX_FRAME;
        e->sequence = seq++;
        e->flags = OBU_EVENT_F_VALID;
        e->source_monotonic_us = c5_to_s3_monotonic(w.meta.rx_monotonic_us);
        e->hub_monotonic_us = obu_monotonic_us();
        e->payload_len = (uint16_t)total;
        memcpy(e->payload, &w.meta, sizeof(w.meta));
        memcpy(e->payload + sizeof(w.meta), frame, w.frame_len);
        stamp_event_utc(e);
        (void)obu_bus_publish(&bus, e);
    } else if (m->type == OBU_IPC_RADIO_STATUS && m->payload_len >= sizeof(obu_ipc_radio_status_t)) {
        obu_ipc_radio_status_t st;
        memcpy(&st, m->payload, sizeof(st));
        c5_status_count++;
        if (st.boot_nonce != 0 && c5_last_boot_nonce != 0 && st.boot_nonce != c5_last_boot_nonce) {
            reset_c5_clock("C5 boot nonce changed");
            publish_clock_status();
        }
        if (st.boot_nonce != 0) c5_last_boot_nonce = st.boot_nonce;
        if (c5_status_count == 1 || (c5_status_count % 5U) == 0) {
            ESP_LOGI(TAG,
                     "C5 status #%u: radio=%s tx_armed=%s freq=%uMHz fw=%s boot_nonce=%u rx=%u drop_buf=%u drop_size=%u ipc_drop=%u tx=%u/%u failed=%u reset=%u",
                     (unsigned)c5_status_count,
                     st.radio_on ? "ON" : "OFF",
                     st.tx_armed ? "YES" : "NO",
                     (unsigned)st.frequency_mhz,
                     st.firmware_version,
                     (unsigned)st.boot_nonce,
                     (unsigned)st.rx_frames,
                     (unsigned)st.rx_drop_buffer,
                     (unsigned)st.rx_drop_size,
                     (unsigned)st.ipc_drop,
                     (unsigned)st.tx_success,
                     (unsigned)st.tx_attempts,
                     (unsigned)st.tx_failed,
                     (unsigned)st.reset_reason);
        }
    } else if (m->type == OBU_IPC_TIME_RESPONSE && m->payload_len >= sizeof(obu_ipc_time_response_t)) {
        obu_ipc_time_response_t tr;
        memcpy(&tr, m->payload, sizeof(tr));
        c5_time_response_count++;
        update_clock_relation(tr.t1_s3_us, tr.t2_c5_us, tr.t3_c5_us, obu_monotonic_us());
        if (c5_time_response_count == 1 || (c5_time_response_count % 5U) == 0) {
            portENTER_CRITICAL(&c5_clock.lock);
            const uint32_t samples = c5_clock.samples;
            const uint32_t rtt = c5_clock.last_rtt_us;
            const int64_t offset = c5_clock.c5_to_s3_offset_us;
            portEXIT_CRITICAL(&c5_clock.lock);
            ESP_LOGI(TAG,
                     "C5 clock sync: responses=%u samples=%u RTT=%uus offset=%lldus",
                     (unsigned)c5_time_response_count,
                     (unsigned)samples,
                     (unsigned)rtt,
                     (long long)offset);
        }
    }
}

static void monitor_c5_link(void)
{
    if (!c5_link_online || c5_last_message_us == 0) return;
    const uint64_t now = obu_monotonic_us();
    if (now - c5_last_message_us > 3000000ULL) {
        c5_link_online = false;
        reset_c5_clock("C5 application link timeout");
        publish_clock_status();
        ESP_LOGW(TAG, "C5 application IPC timed out after %llu ms",
                 (unsigned long long)((now - c5_last_message_us) / 1000ULL));
    }
    if (now - c5_last_link_report_us > 10000000ULL) {
        c5_last_link_report_us = now;
        ESP_LOGI(TAG,
                 "C5 application link: %s messages=%u status=%u time_rsp=%u v2x_frames=%u last_age=%llu ms",
                 c5_link_online ? "ACTIVE" : "INACTIVE",
                 (unsigned)c5_message_count,
                 (unsigned)c5_status_count,
                 (unsigned)c5_time_response_count,
                 (unsigned)c5_rx_frame_count,
                 c5_last_message_us == 0 ? 0ULL :
                 (unsigned long long)((now - c5_last_message_us) / 1000ULL));
    }
}

static void event_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        if (xQueueReceive(event_queue, &event_consumer_scratch, portMAX_DELAY) != pdTRUE) continue;
        if (logger) (void)obu_diag_logger_submit(logger, &event_consumer_scratch);
    }
}

static void IRAM_ATTR pps_isr(void *arg)
{
    (void)arg;
    uint64_t now = obu_monotonic_us();
    BaseType_t hp = pdFALSE;
    if (pps_queue) xQueueSendFromISR(pps_queue, &now, &hp);
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
    const gpio_num_t pps_gpio = (gpio_num_t)CONFIG_OBU_GNSS_PPS_GPIO;
    pps_queue = xQueueCreate(4, sizeof(uint64_t));
    if (!pps_queue) return ESP_ERR_NO_MEM;
    gpio_config_t gc = {
        .pin_bit_mask = 1ULL << pps_gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&gc), TAG, "PPS gpio");
    esp_err_t e = gpio_install_isr_service(0);
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(pps_gpio, pps_isr, NULL), TAG, "PPS isr");
    if (xTaskCreate(pps_task, "gnss_pps", 3072, NULL, 11, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "GNSS PPS input: GPIO%d", (int)pps_gpio);
    return ESP_OK;
}

static void publish_startup_status(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    char text[128];
    snprintf(text, sizeof(text), "S3 boot reset=%u fw=%s", (unsigned)esp_reset_reason(), d ? d->version : "unknown");

    obu_event_t *e = prepare_main_event();
    e->source = OBU_SOURCE_SYSTEM;
    e->type = OBU_DATA_DIAGNOSTIC;
    e->sequence = seq++;
    e->flags = OBU_EVENT_F_VALID;
    e->source_monotonic_us = obu_monotonic_us();
    e->hub_monotonic_us = e->source_monotonic_us;
    e->payload_len = (uint16_t)strnlen(text, sizeof(text));
    memcpy(e->payload, text, e->payload_len);
    stamp_event_utc(e);
    (void)obu_bus_publish(&bus, e);
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
    if (display_err != ESP_OK) ESP_LOGW(TAG, "OLED unavailable; acquisition continues: %s", esp_err_to_name(display_err));
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
        ESP_LOGW(TAG, "local warning buzzer unavailable; acquisition continues: %s", esp_err_to_name(buzzer_err));
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
    ESP_LOGI(TAG,
             "C5 SPI wiring expected: S3 GPIO7 SCLK -> C5 GPIO8; S3 GPIO9 MOSI -> C5 GPIO10; S3 GPIO8 MISO <- C5 GPIO9; S3 GPIO1 CS -> C5 GPIO1; common GND; 8MHz mode0");
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

#ifdef CONFIG_OBU_LORAWAN_ENABLE
    hmi.lorawan_enabled = true;
    const obu_lorawan_config_t lorawan_config = {
        .enabled = true,
        .host = SPI2_HOST,
        .sck_gpio = 7,
        .miso_gpio = 8,
        .mosi_gpio = 9,
        .nss_gpio = 41,
        .dio1_gpio = 39,
        .reset_gpio = 42,
        .busy_gpio = 40,
        .spi_clock_hz = 2000000,
        .join_eui_hex = CONFIG_OBU_LORAWAN_JOIN_EUI,
        .dev_eui_hex = CONFIG_OBU_LORAWAN_DEV_EUI,
        .nwk_key_hex = CONFIG_OBU_LORAWAN_NWK_KEY,
        .app_key_hex = CONFIG_OBU_LORAWAN_APP_KEY,
        .join_datarate = CONFIG_OBU_LORAWAN_JOIN_DATARATE,
        .fport = CONFIG_OBU_LORAWAN_FPORT,
        .max_frame_bytes = CONFIG_OBU_LORAWAN_MAX_FRAME_BYTES,
        .fragment_data_bytes = CONFIG_OBU_LORAWAN_FRAGMENT_DATA_BYTES,
        .queue_depth = CONFIG_OBU_LORAWAN_QUEUE_DEPTH,
        .min_fragment_interval_ms = CONFIG_OBU_LORAWAN_MIN_FRAGMENT_INTERVAL_MS,
        .join_retry_ms = CONFIG_OBU_LORAWAN_JOIN_RETRY_MS,
    };
    const esp_err_t lorawan_err = obu_lorawan_start(&lorawan_config, &lorawan);
    hmi.lorawan_ready = lorawan_err == ESP_OK && lorawan != NULL;
    if (lorawan_err != ESP_OK) {
        ESP_LOGW(TAG, "Wio-SX1262 LoRaWAN uplink unavailable; acquisition continues: %s",
                 esp_err_to_name(lorawan_err));
    }
#endif

    publish_startup_status();
    if (display.ops != NULL && xTaskCreate(hmi_task, "hmi", 4096, NULL, 5, NULL) != pdPASS) abort();
    if (xTaskCreate(time_probe_task, "c5_clock", 3072, NULL, 8, NULL) != pdPASS) abort();

    for (;;) {
        if (obu_ipc_receive(ipc, &main_ipc_rx, pdMS_TO_TICKS(250)) == ESP_OK) ingest_ipc(&main_ipc_rx);
        monitor_c5_link();
    }
}
