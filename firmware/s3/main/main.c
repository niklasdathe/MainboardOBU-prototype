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
#include "obu_time.h"
#include "obu_v2x.h"
#include "obu_warning.h"

static const char *TAG = "s3_main";
static obu_bus_t bus;
static obu_ipc_endpoint_t *ipc;
static obu_display_driver_t display;
static obu_time_service_t timesvc;
static obu_diag_logger_t *logger;
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
    obu_v2x_frame_info_t info;
    if (!obu_v2x_classify_80211_frame(frame, frame_len, &info)) return;

    hmi.v2x_rx_seen = true;
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

        size_t total = sizeof(w.meta) + w.frame_len;
        if (total > OBU_EVENT_PAYLOAD_MAX) {
            ESP_LOGW(TAG, "C5 RX_FRAME too large for event bus: %u bytes", (unsigned)total);
            return;
        }

        const uint8_t *frame = m->payload + sizeof(w);
        update_v2x_hmi_and_geiger(frame, w.frame_len);

        c5_rx_frame_count++;
        if (c5_rx_frame_count == 1u || (c5_rx_frame_count % 100u) == 0u) {
            ESP_LOGI(TAG,
                     "C5 V2X RX #%u: c5_seq=%u len=%u freq=%uMHz rssi=%ddBm hw_ts=%llu us",
                     (unsigned)c5_rx_frame_count,
                     (unsigned)w.meta.c5_sequence,
                     (unsigned)w.frame_len,
                     (unsigned)w.meta.frequency_mhz,
                     (int)w.meta.rssi_dbm,
                     (unsigned long long)w.meta.radio_hw_timestamp_us);
        }

        obu_event_t *e = prepare_main_event();
        e->source = OBU_SOURCE_C5_RADIO;
        e->type = OBU_DATA_V2X_RAW_RX;
        e->sequence = seq++;
        e->flags = OBU_EVENT_F_RAW | OBU_EVENT_F_VALID;
        e->source_monotonic_us = w.meta.c5_rx_monotonic_us;
        e->hub_monotonic_us = c5_to_s3_monotonic(w.meta.c5_rx_monotonic_us);
        e->validity_ms = 0;
        e->payload_len = (uint16_t)total;
        memcpy(e->payload, &w.meta, sizeof(w.meta));
        memcpy(e->payload + sizeof(w.meta), frame, w.frame_len);
        stamp_event_utc(e);
        (void)obu_bus_publish(&bus, e);
    } else if (m->type == OBU_IPC_RADIO_STATUS && m->payload_len >= sizeof(obu_radio_status_t)) {
        obu_radio_status_t s;
        memcpy(&s, m->payload, sizeof(s));
        const bool boot_changed = c5_status_count != 0u && c5_last_boot_nonce != s.boot_nonce;
        const bool state_changed = c5_status_count == 0u || boot_changed || hmi.c5_online != s.radio_running;
        if (boot_changed) {
            ESP_LOGW(TAG, "C5 reboot detected: boot_nonce %u -> %u; discarding old clock relation",
                     (unsigned)c5_last_boot_nonce, (unsigned)s.boot_nonce);
            reset_c5_clock("C5 boot nonce changed");
            publish_clock_status();
        }
        c5_status_count++;
        c5_last_boot_nonce = s.boot_nonce;
        hmi.c5_online = s.radio_running;

        if (state_changed || (c5_status_count % 5u) == 0u) {
            ESP_LOGI(TAG,
                     "C5 status #%u: radio=%s tx_armed=%s freq=%uMHz fw=%s boot_nonce=%u rx=%u drop_buf=%u drop_size=%u ipc_drop=%u tx=%u/%u failed=%u reset=%u",
                     (unsigned)c5_status_count,
                     s.radio_running ? "ON" : "OFF",
                     s.tx_armed ? "YES" : "NO",
                     (unsigned)s.frequency_mhz,
                     s.firmware_version,
                     (unsigned)s.boot_nonce,
                     (unsigned)s.rx_frames,
                     (unsigned)s.rx_drop_no_buffer,
                     (unsigned)s.rx_drop_oversize,
                     (unsigned)s.ipc_drop,
                     (unsigned)s.tx_success,
                     (unsigned)s.tx_requests,
                     (unsigned)s.tx_failed,
                     (unsigned)s.reset_reason);
        }

        obu_event_t *e = prepare_main_event();
        e->source = OBU_SOURCE_C5_RADIO;
        e->type = OBU_DATA_RADIO_STATUS;
        e->sequence = seq++;
        e->flags = OBU_EVENT_F_VALID;
        e->source_monotonic_us = m->source_monotonic_us;
        e->hub_monotonic_us = c5_to_s3_monotonic(m->source_monotonic_us);
        e->payload_len = sizeof(s);
        memcpy(e->payload, &s, sizeof(s));
        stamp_event_utc(e);
        (void)obu_bus_publish(&bus, e);
    } else if (m->type == OBU_IPC_TX_RESULT) {
        ESP_LOGI(TAG, "C5 TX result received: seq=%u payload=%u", (unsigned)m->sequence, (unsigned)m->payload_len);
        obu_event_t *e = prepare_main_event();
        e->source = OBU_SOURCE_C5_RADIO;
        e->type = OBU_DATA_V2X_TX_RESULT;
        e->sequence = seq++;
        e->flags = OBU_EVENT_F_VALID;
        e->source_monotonic_us = m->source_monotonic_us;
        e->hub_monotonic_us = c5_to_s3_monotonic(m->source_monotonic_us);
        e->payload_len = m->payload_len;
        memcpy(e->payload, m->payload, m->payload_len);
        stamp_event_utc(e);
        (void)obu_bus_publish(&bus, e);
    } else if (m->type == OBU_IPC_TIME_RESPONSE && m->payload_len >= 24) {
        uint64_t t1, t2, t3;
        memcpy(&t1, m->payload, 8);
        memcpy(&t2, m->payload + 8, 8);
        memcpy(&t3, m->payload + 16, 8);
        update_clock_relation(t1, t2, t3, obu_monotonic_us());
        c5_time_response_count++;
        if (c5_time_response_count == 1u || (c5_time_response_count % 5u) == 0u) {
            int64_t offset;
            uint32_t rtt;
            uint32_t samples;
            portENTER_CRITICAL(&c5_clock.lock);
            offset = c5_clock.c5_to_s3_offset_us;
            rtt = c5_clock.last_rtt_us;
            samples = c5_clock.samples;
            portEXIT_CRITICAL(&c5_clock.lock);
            ESP_LOGI(TAG, "C5 clock sync: responses=%u samples=%u RTT=%uus offset=%lldus",
                     (unsigned)c5_time_response_count, (unsigned)samples, (unsigned)rtt, (long long)offset);
        }
        publish_clock_status();
    } else {
        ESP_LOGW(TAG, "Unhandled C5 IPC message type=%s(%u) payload=%u",
                 ipc_type_name(m->type), (unsigned)m->type, (unsigned)m->payload_len);
    }
}

static void monitor_c5_link(void)
{
    const uint64_t now = obu_monotonic_us();
    if (c5_last_message_us == 0) {
        if (now > 3000000ULL && (c5_last_link_report_us == 0 || now - c5_last_link_report_us >= 5000000ULL)) {
            c5_last_link_report_us = now;
            ESP_LOGW(TAG,
                     "No application message from C5 yet. Check power/GND and SPI wiring: S3 GPIO7(SCLK)->C5 GPIO8, GPIO9(MOSI)->C5 GPIO10, GPIO8(MISO)<-C5 GPIO9, GPIO1(CS)->C5 GPIO1");
        }
        return;
    }

    const uint64_t age_us = now - c5_last_message_us;
    if (age_us > 3000000ULL && c5_link_online) {
        c5_link_online = false;
        hmi.c5_online = false;
        reset_c5_clock("C5 application link timeout");
        publish_clock_status();
        ESP_LOGW(TAG, "C5 application IPC timeout: no message for %llu ms; waiting for automatic recovery",
                 (unsigned long long)(age_us / 1000ULL));
    }

    if (now - c5_last_link_report_us >= 10000000ULL) {
        c5_last_link_report_us = now;
        ESP_LOGI(TAG, "C5 application link: %s messages=%u status=%u time_rsp=%u v2x_frames=%u last_age=%llu ms",
                 c5_link_online ? "ACTIVE" : "STALE",
                 (unsigned)c5_message_count,
                 (unsigned)c5_status_count,
                 (unsigned)c5_time_response_count,
                 (unsigned)c5_rx_frame_count,
                 (unsigned long long)(age_us / 1000ULL));
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

    if (e->payload_len >= sizeof(warning)) memcpy(&warning, e->payload, sizeof(warning));

    hmi.warning = warning.kind == OBU_WARNING_KIND_SYSTEM ? OBU_HMI_WARN_SYSTEM : OBU_HMI_WARN_DENM;
    snprintf(hmi.warning_text, sizeof(hmi.warning_text), "%s", warning.text[0] ? warning.text : "V2X WARNING");

    if (warning_output_ready) {
        esp_err_t err = obu_warning_controller_handle(&warning_controller, &warning);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) ESP_LOGW(TAG, "warning output failed: %s", esp_err_to_name(err));
    }
}

static void event_consumer(void *arg)
{
    (void)arg;
    for (;;) {
        if (xQueueReceive(event_queue, &event_consumer_scratch, portMAX_DELAY) != pdTRUE) continue;
        obu_event_t *e = &event_consumer_scratch;
        process_gnss(e);
        if (!e->utc_ns) stamp_event_utc(e);
        if (logger && (e->type == OBU_DATA_DIAGNOSTIC || e->type == OBU_DATA_RADIO_STATUS ||
                       e->type == OBU_DATA_V2X_TX_RESULT || e->type == OBU_DATA_CLOCK_SYNC || !(e->flags & OBU_EVENT_F_VALID))) {
            (void)obu_diag_log_event(logger, e, "event");
        }
        if (e->type == OBU_DATA_WARNING) process_warning(e);
    }
}

static void hmi_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (display.ops != NULL && display.ops->render != NULL) (void)display.ops->render(&display, &hmi);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void time_probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        memset(&time_probe_message, 0, sizeof(time_probe_message));
        time_probe_message.type = OBU_IPC_TIME_PROBE;
        time_probe_message.sequence = ipc_seq++;
        time_probe_message.source_monotonic_us = obu_monotonic_us();
        time_probe_message.payload_len = 8;
        memcpy(time_probe_message.payload, &time_probe_message.source_monotonic_us, 8);
        const esp_err_t err = obu_ipc_send(ipc, &time_probe_message, pdMS_TO_TICKS(5));
        if (err != ESP_OK) ESP_LOGW(TAG, "Failed to queue C5 time probe: %s", esp_err_to_name(err));
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

    publish_startup_status();
    if (display.ops != NULL && xTaskCreate(hmi_task, "hmi", 4096, NULL, 5, NULL) != pdPASS) abort();
    if (xTaskCreate(time_probe_task, "c5_clock", 3072, NULL, 8, NULL) != pdPASS) abort();

    for (;;) {
        if (obu_ipc_receive(ipc, &main_ipc_rx, pdMS_TO_TICKS(250)) == ESP_OK) ingest_ipc(&main_ipc_rx);
        monitor_c5_link();
    }
}