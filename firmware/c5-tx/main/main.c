#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "obu_radio.h"
#include "sdkconfig.h"

#define TX_FRAME_MAX 2400u
#define TX_PDU_MAX 2200u
#define IEEE80211_HEADER_LEN 24u
#define LLC_SNAP_LEN 8u
#define GN_BASIC_HEADER_LEN 4u
#define GN_COMMON_HEADER_LEN 8u
#define GN_SHB_EXT_LEN 28u
#define GN_GBC_EXT_LEN 44u
#define BTP_HEADER_LEN 4u
#define ITS_PDU_HEADER_LEN 6u
#define ITS_EPOCH_UNIX_S 1072915200ULL
#define BOOT_DEBOUNCE_US 30000ULL
#define MAIN_POLL_MS 20u

#define GN_BASIC_NEXT_COMMON 1u
#define GN_COMMON_NEXT_BTP_B 2u
#define GN_HEADER_TYPE_SHB 0x50u
#define GN_HEADER_TYPE_GBC 0x40u

static const char *TAG = "c5_tx";
static obu_radio_t *radio;
static uint8_t frame_buffer[TX_FRAME_MAX];
static uint8_t pdu_buffer[TX_PDU_MAX];
static uint16_t mac_sequence;
static uint16_t gn_sequence;
static uint32_t tx_ok;
static uint32_t tx_failed;
static uint8_t source_mac[6];

typedef enum {
    GN_MODE_SHB = 0,
    GN_MODE_GEOBROADCAST,
} gn_mode_t;

typedef struct {
    const char *name;
    uint8_t protocol_version;
    uint8_t message_id;
    uint16_t btp_port;
    gn_mode_t gn_mode;
    bool enabled;
    bool button;
    bool periodic;
    bool patch_generation_delta_time;
    uint32_t interval_ms;
    const char *pdu_hex;
    uint64_t next_due_us;
} tx_profile_t;

#if defined(CONFIG_C5_TX_CAM_ENABLE)
#define CAM_ENABLED true
#else
#define CAM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_CAM_BUTTON)
#define CAM_BUTTON true
#else
#define CAM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_CAM_PERIODIC)
#define CAM_PERIODIC true
#else
#define CAM_PERIODIC false
#endif
#if defined(CONFIG_C5_TX_VAM_ENABLE)
#define VAM_ENABLED true
#else
#define VAM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_VAM_BUTTON)
#define VAM_BUTTON true
#else
#define VAM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_VAM_PERIODIC)
#define VAM_PERIODIC true
#else
#define VAM_PERIODIC false
#endif
#if defined(CONFIG_C5_TX_DENM_ENABLE)
#define DENM_ENABLED true
#else
#define DENM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_DENM_BUTTON)
#define DENM_BUTTON true
#else
#define DENM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_DENM_PERIODIC)
#define DENM_PERIODIC true
#else
#define DENM_PERIODIC false
#endif
#if defined(CONFIG_C5_TX_CPM_ENABLE)
#define CPM_ENABLED true
#else
#define CPM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_CPM_BUTTON)
#define CPM_BUTTON true
#else
#define CPM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_CPM_PERIODIC)
#define CPM_PERIODIC true
#else
#define CPM_PERIODIC false
#endif
#if defined(CONFIG_C5_TX_SPATEM_ENABLE)
#define SPATEM_ENABLED true
#else
#define SPATEM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_SPATEM_BUTTON)
#define SPATEM_BUTTON true
#else
#define SPATEM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_SPATEM_PERIODIC)
#define SPATEM_PERIODIC true
#else
#define SPATEM_PERIODIC false
#endif
#if defined(CONFIG_C5_TX_MAPEM_ENABLE)
#define MAPEM_ENABLED true
#else
#define MAPEM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_MAPEM_BUTTON)
#define MAPEM_BUTTON true
#else
#define MAPEM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_MAPEM_PERIODIC)
#define MAPEM_PERIODIC true
#else
#define MAPEM_PERIODIC false
#endif
#if defined(CONFIG_C5_TX_IVIM_ENABLE)
#define IVIM_ENABLED true
#else
#define IVIM_ENABLED false
#endif
#if defined(CONFIG_C5_TX_IVIM_BUTTON)
#define IVIM_BUTTON true
#else
#define IVIM_BUTTON false
#endif
#if defined(CONFIG_C5_TX_IVIM_PERIODIC)
#define IVIM_PERIODIC true
#else
#define IVIM_PERIODIC false
#endif

/*
 * Release-2 message identifiers and the corresponding well-known BTP ports.
 * The Facilities bytes themselves are supplied as UPER hex through menuconfig.
 */
static tx_profile_t profiles[] = {
    {.name = "CAM", .protocol_version = 2, .message_id = 2, .btp_port = 2001,
     .gn_mode = GN_MODE_SHB, .enabled = CAM_ENABLED, .button = CAM_BUTTON,
     .periodic = CAM_PERIODIC, .patch_generation_delta_time = true,
     .interval_ms = CONFIG_C5_TX_CAM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_CAM_PDU_HEX},
    {.name = "VAM", .protocol_version = 3, .message_id = 16, .btp_port = 2018,
     .gn_mode = GN_MODE_SHB, .enabled = VAM_ENABLED, .button = VAM_BUTTON,
     .periodic = VAM_PERIODIC, .patch_generation_delta_time = true,
     .interval_ms = CONFIG_C5_TX_VAM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_VAM_PDU_HEX},
    {.name = "DENM", .protocol_version = 2, .message_id = 1, .btp_port = 2002,
     .gn_mode = GN_MODE_GEOBROADCAST, .enabled = DENM_ENABLED, .button = DENM_BUTTON,
     .periodic = DENM_PERIODIC, .patch_generation_delta_time = false,
     .interval_ms = CONFIG_C5_TX_DENM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_DENM_PDU_HEX},
    {.name = "CPM", .protocol_version = 2, .message_id = 14, .btp_port = 2009,
     .gn_mode = GN_MODE_SHB, .enabled = CPM_ENABLED, .button = CPM_BUTTON,
     .periodic = CPM_PERIODIC, .patch_generation_delta_time = false,
     .interval_ms = CONFIG_C5_TX_CPM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_CPM_PDU_HEX},
    {.name = "SPATEM", .protocol_version = 2, .message_id = 4, .btp_port = 2004,
     .gn_mode = GN_MODE_SHB, .enabled = SPATEM_ENABLED, .button = SPATEM_BUTTON,
     .periodic = SPATEM_PERIODIC, .patch_generation_delta_time = false,
     .interval_ms = CONFIG_C5_TX_SPATEM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_SPATEM_PDU_HEX},
    {.name = "MAPEM", .protocol_version = 2, .message_id = 5, .btp_port = 2003,
     .gn_mode = GN_MODE_SHB, .enabled = MAPEM_ENABLED, .button = MAPEM_BUTTON,
     .periodic = MAPEM_PERIODIC, .patch_generation_delta_time = false,
     .interval_ms = CONFIG_C5_TX_MAPEM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_MAPEM_PDU_HEX},
    {.name = "IVIM", .protocol_version = 2, .message_id = 6, .btp_port = 2006,
     .gn_mode = GN_MODE_SHB, .enabled = IVIM_ENABLED, .button = IVIM_BUTTON,
     .periodic = IVIM_PERIODIC, .patch_generation_delta_time = false,
     .interval_ms = CONFIG_C5_TX_IVIM_INTERVAL_MS, .pdu_hex = CONFIG_C5_TX_IVIM_PDU_HEX},
};

static void write_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static void write_be32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static void write_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static bool parse_mac(const char *text, uint8_t out[6])
{
    unsigned int b[6];
    if (text == NULL || sscanf(text, "%x:%x:%x:%x:%x:%x",
                               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) != 6) {
        return false;
    }
    for (size_t i = 0; i < 6; ++i) {
        if (b[i] > 0xffu) return false;
        out[i] = (uint8_t)b[i];
    }
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool parse_hex(const char *text, uint8_t *out, size_t capacity, size_t *out_len)
{
    int high = -1;
    size_t length = 0;
    if (text == NULL || out == NULL || out_len == NULL) return false;

    for (const char *p = text; *p != '\0'; ++p) {
        if (isspace((unsigned char)*p) || *p == ':' || *p == '-' || *p == '_') continue;
        const int value = hex_value(*p);
        if (value < 0) return false;
        if (high < 0) {
            high = value;
        } else {
            if (length >= capacity) return false;
            out[length++] = (uint8_t)((high << 4) | value);
            high = -1;
        }
    }
    if (high >= 0) return false;
    *out_len = length;
    return true;
}

static bool its_time_ms(uint64_t *out_ms)
{
    if (out_ms == NULL || CONFIG_C5_TX_UNIX_TIME_AT_BOOT <= (int)ITS_EPOCH_UNIX_S) return false;
    const uint64_t boot_its_ms = ((uint64_t)CONFIG_C5_TX_UNIX_TIME_AT_BOOT - ITS_EPOCH_UNIX_S) * 1000ULL;
    *out_ms = boot_its_ms + (uint64_t)(esp_timer_get_time() / 1000LL);
    return true;
}

static uint32_t gn_timestamp_ms(void)
{
    uint64_t value;
    if (its_time_ms(&value)) return (uint32_t)value;
    return (uint32_t)((uint64_t)esp_timer_get_time() / 1000ULL);
}

static void fill_long_position_vector(uint8_t *out)
{
    const uint16_t gn_address_prefix =
        (uint16_t)(0x8000u | (((uint16_t)CONFIG_C5_TX_GN_STATION_TYPE & 0x1fu) << 10));
    write_be16(out, gn_address_prefix);
    memcpy(out + 2, source_mac, 6);
    write_be32(out + 8, gn_timestamp_ms());
    write_be32(out + 12, (uint32_t)(int32_t)CONFIG_C5_TX_LATITUDE_1E7);
    write_be32(out + 16, (uint32_t)(int32_t)CONFIG_C5_TX_LONGITUDE_1E7);

    uint16_t speed = (uint16_t)CONFIG_C5_TX_SPEED_CM_S & 0x7fffu;
#ifdef CONFIG_C5_TX_POSITION_ACCURATE
    speed |= 0x8000u;
#endif
    write_be16(out + 20, speed);
    write_be16(out + 22, (uint16_t)CONFIG_C5_TX_HEADING_DEG10);
}

static bool load_facilities_pdu(tx_profile_t *profile, size_t *out_len)
{
    size_t length = 0;
    const bool configured = profile->pdu_hex != NULL && profile->pdu_hex[0] != '\0';

    if (configured) {
        if (!parse_hex(profile->pdu_hex, pdu_buffer, sizeof(pdu_buffer), &length)) {
            ESP_LOGE(TAG, "%s PDU hex is malformed or too large", profile->name);
            return false;
        }
    } else {
#ifdef CONFIG_C5_TX_ALLOW_HEADER_ONLY_PROBES
        length = ITS_PDU_HEADER_LEN;
        pdu_buffer[0] = profile->protocol_version;
        pdu_buffer[1] = profile->message_id;
        write_be32(pdu_buffer + 2, (uint32_t)CONFIG_C5_TX_STATION_ID);
        ESP_LOGW(TAG, "%s uses a header-only diagnostic probe; this is NOT a valid Facilities message",
                 profile->name);
#else
        ESP_LOGD(TAG, "%s skipped: no Facilities UPER PDU configured", profile->name);
        return false;
#endif
    }

    if (length < ITS_PDU_HEADER_LEN) {
        ESP_LOGE(TAG, "%s PDU is shorter than ItsPduHeader", profile->name);
        return false;
    }
    if (pdu_buffer[0] != profile->protocol_version || pdu_buffer[1] != profile->message_id) {
        ESP_LOGE(TAG, "%s PDU header mismatch: expected protocolVersion=%u messageId=%u, got %u/%u",
                 profile->name, (unsigned)profile->protocol_version, (unsigned)profile->message_id,
                 (unsigned)pdu_buffer[0], (unsigned)pdu_buffer[1]);
        return false;
    }

#ifdef CONFIG_C5_TX_PATCH_STATION_ID
    write_be32(pdu_buffer + 2, (uint32_t)CONFIG_C5_TX_STATION_ID);
#endif

#if defined(CONFIG_C5_TX_PATCH_GENERATION_DELTA_TIME)
    if (profile->patch_generation_delta_time && length >= ITS_PDU_HEADER_LEN + 2u) {
        uint64_t now_its_ms;
        if (its_time_ms(&now_its_ms)) {
            write_be16(pdu_buffer + ITS_PDU_HEADER_LEN, (uint16_t)(now_its_ms & 0xffffu));
        }
    }
#endif

    *out_len = length;
    return true;
}

static size_t build_frame(const tx_profile_t *profile, const uint8_t *pdu, size_t pdu_len)
{
    const size_t ext_len = profile->gn_mode == GN_MODE_GEOBROADCAST ? GN_GBC_EXT_LEN : GN_SHB_EXT_LEN;
    const size_t total = IEEE80211_HEADER_LEN + LLC_SNAP_LEN + GN_BASIC_HEADER_LEN +
                         GN_COMMON_HEADER_LEN + ext_len + BTP_HEADER_LEN + pdu_len;
    if (pdu == NULL || total > sizeof(frame_buffer) || pdu_len + BTP_HEADER_LEN > 0xffffu) return 0;

    uint8_t *p = frame_buffer;
    memset(frame_buffer, 0, total);

    /* IEEE 802.11 data frame in OCB-style broadcast form. FCS is generated by the radio. */
    p[0] = 0x08;
    p[1] = 0x00;
    memset(p + 4, 0xff, 6);          /* Address 1: broadcast */
    memcpy(p + 10, source_mac, 6);   /* Address 2: transmitter */
    memset(p + 16, 0xff, 6);         /* Address 3: wildcard BSSID */
    write_le16(p + 22, (uint16_t)((mac_sequence++ & 0x0fffu) << 4));
    p += IEEE80211_HEADER_LEN;

    /* LLC/SNAP with the GeoNetworking EtherType 0x8947. */
    static const uint8_t llc[LLC_SNAP_LEN] = {0xaa, 0xaa, 0x03, 0x00, 0x00, 0x00, 0x89, 0x47};
    memcpy(p, llc, sizeof(llc));
    p += sizeof(llc);

    /* GeoNetworking Basic Header: version 1, Common Header follows. */
    p[0] = (uint8_t)((1u << 4) | GN_BASIC_NEXT_COMMON);
    p[1] = 0;
    p[2] = 0x05; /* finite packet lifetime encoding for local test traffic */
    p[3] = profile->gn_mode == GN_MODE_GEOBROADCAST ? 10u : 1u;
    p += GN_BASIC_HEADER_LEN;

    /* GeoNetworking Common Header: BTP-B; SHB or GeoBroadcast. */
    p[0] = (uint8_t)(GN_COMMON_NEXT_BTP_B << 4);
    p[1] = profile->gn_mode == GN_MODE_GEOBROADCAST
               ? (uint8_t)(GN_HEADER_TYPE_GBC | (CONFIG_C5_TX_GBC_SHAPE & 0x0fu))
               : GN_HEADER_TYPE_SHB;
    p[2] = 0; /* traffic class: best effort; keep SCF disabled */
    p[3] = 0; /* flags */
    write_be16(p + 4, (uint16_t)(BTP_HEADER_LEN + pdu_len));
    p[6] = profile->gn_mode == GN_MODE_GEOBROADCAST ? 10u : 1u;
    p[7] = 0;
    p += GN_COMMON_HEADER_LEN;

    if (profile->gn_mode == GN_MODE_GEOBROADCAST) {
        write_be16(p, gn_sequence++);
        write_be16(p + 2, 0);
        fill_long_position_vector(p + 4);
        write_be32(p + 28, (uint32_t)(int32_t)CONFIG_C5_TX_LATITUDE_1E7);
        write_be32(p + 32, (uint32_t)(int32_t)CONFIG_C5_TX_LONGITUDE_1E7);
        write_be16(p + 36, (uint16_t)CONFIG_C5_TX_GBC_DISTANCE_A_M);
        write_be16(p + 38, (uint16_t)CONFIG_C5_TX_GBC_DISTANCE_B_M);
        write_be16(p + 40, (uint16_t)CONFIG_C5_TX_GBC_ANGLE_DEG10);
        write_be16(p + 42, 0);
    } else {
        fill_long_position_vector(p);
        write_be32(p + 24, 0);
    }
    p += ext_len;

    /* BTP-B: destination port and destination-port-info. */
    write_be16(p, profile->btp_port);
    write_be16(p + 2, 0);
    p += BTP_HEADER_LEN;

    memcpy(p, pdu, pdu_len);
    return total;
}

static void led_pulse(void)
{
    gpio_set_level((gpio_num_t)CONFIG_C5_TX_LED_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(15));
    gpio_set_level((gpio_num_t)CONFIG_C5_TX_LED_GPIO, 1);
}

static bool transmit_profile(tx_profile_t *profile, const char *reason)
{
    size_t pdu_len;
    if (!profile->enabled || !load_facilities_pdu(profile, &pdu_len)) return false;

    const size_t frame_len = build_frame(profile, pdu_buffer, pdu_len);
    if (frame_len == 0) {
        ESP_LOGE(TAG, "%s frame construction failed", profile->name);
        tx_failed++;
        return false;
    }

    const esp_err_t err = obu_radio_transmit(radio, frame_buffer, frame_len);
    if (err == ESP_OK) {
        tx_ok++;
        ESP_LOGI(TAG, "TX %s reason=%s frame=%uB pdu=%uB BTP=%u GN=%s ok=%u failed=%u",
                 profile->name, reason, (unsigned)frame_len, (unsigned)pdu_len,
                 (unsigned)profile->btp_port,
                 profile->gn_mode == GN_MODE_GEOBROADCAST ? "GeoBroadcast" : "SHB",
                 (unsigned)tx_ok, (unsigned)tx_failed);
        led_pulse();
        return true;
    }

    tx_failed++;
    ESP_LOGE(TAG, "TX %s failed: %s", profile->name, esp_err_to_name(err));
    return false;
}

static void transmit_button_profiles(const char *reason)
{
    bool sent_any = false;
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        if (!profiles[i].enabled || !profiles[i].button) continue;
        if (sent_any) vTaskDelay(pdMS_TO_TICKS(CONFIG_C5_TX_INTERFRAME_GAP_MS));
        sent_any |= transmit_profile(&profiles[i], reason);
    }
    if (!sent_any) {
        ESP_LOGW(TAG, "%s trigger produced no frame; configure at least one enabled button profile with UPER hex",
                 reason);
    }
}

static void init_gpio(void)
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << CONFIG_C5_TX_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));

    const gpio_config_t led = {
        .pin_bit_mask = 1ULL << CONFIG_C5_TX_LED_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level((gpio_num_t)CONFIG_C5_TX_LED_GPIO, 1);
}

static void init_periodic_deadlines(uint64_t now_us)
{
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        profiles[i].next_due_us = now_us + (uint64_t)profiles[i].interval_ms * 1000ULL;
    }
}

static void service_periodic(uint64_t now_us)
{
#ifdef CONFIG_C5_TX_PERIODIC_ENABLE
    for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i) {
        tx_profile_t *profile = &profiles[i];
        if (!profile->enabled || !profile->periodic || now_us < profile->next_due_us) continue;
        profile->next_due_us = now_us + (uint64_t)profile->interval_ms * 1000ULL;
        transmit_profile(profile, "periodic");
    }
#else
    (void)now_us;
#endif
}

void app_main(void)
{
#ifndef CONFIG_C5_TX_ENABLED
    ESP_LOGW(TAG, "C5 TX is disabled in menuconfig");
    for (;;) vTaskDelay(portMAX_DELAY);
#else
    if (!parse_mac(CONFIG_C5_TX_SOURCE_MAC, source_mac)) {
        ESP_LOGE(TAG, "Invalid C5_TX_SOURCE_MAC: %s", CONFIG_C5_TX_SOURCE_MAC);
        return;
    }

    init_gpio();

    obu_radio_config_t radio_config = {
        .frequency_mhz = CONFIG_C5_TX_FREQUENCY_MHZ,
        .rx_cb = NULL,
        .rx_ctx = NULL,
    };
    ESP_ERROR_CHECK(obu_radio_create(&radio_config, &radio));
    ESP_ERROR_CHECK(obu_radio_start(radio));
    const uint32_t nonce = obu_radio_boot_nonce(radio);
    ESP_ERROR_CHECK(obu_radio_arm_tx(radio, nonce, true));

    ESP_LOGW(TAG, "Standalone research TX armed for this boot: frequency=%dMHz boot_nonce=%u",
             CONFIG_C5_TX_FREQUENCY_MHZ, (unsigned)nonce);
    ESP_LOGI(TAG, "XIAO C5 BOOT trigger GPIO=%d (active low), LED GPIO=%d",
             CONFIG_C5_TX_BOOT_GPIO, CONFIG_C5_TX_LED_GPIO);
    ESP_LOGI(TAG, "Profiles: CAM/BTP2001, DENM/BTP2002, MAPEM/2003, SPATEM/2004, IVIM/2006, CPM/2009, VAM/2018");

    if (CONFIG_C5_TX_UNIX_TIME_AT_BOOT == 0) {
        ESP_LOGW(TAG, "No UTC boot time configured: GN timestamps are uptime-based; do not use this run as timing-conformance evidence");
    }
#ifndef CONFIG_C5_TX_ALLOW_HEADER_ONLY_PROBES
    ESP_LOGI(TAG, "Header-only probes are disabled; profiles without valid configured UPER PDUs are skipped");
#else
    ESP_LOGW(TAG, "NON-CONFORMANT header-only probe mode is enabled");
#endif

    uint64_t now_us = (uint64_t)esp_timer_get_time();
    init_periodic_deadlines(now_us);

#ifdef CONFIG_C5_TX_STARTUP_TRIGGER
    vTaskDelay(pdMS_TO_TICKS(250));
    transmit_button_profiles("startup");
#endif

    bool raw_pressed = gpio_get_level((gpio_num_t)CONFIG_C5_TX_BOOT_GPIO) == 0;
    bool stable_pressed = raw_pressed;
    uint64_t raw_changed_us = (uint64_t)esp_timer_get_time();

    for (;;) {
        now_us = (uint64_t)esp_timer_get_time();
        service_periodic(now_us);

#ifdef CONFIG_C5_TX_BUTTON_TRIGGER
        const bool pressed = gpio_get_level((gpio_num_t)CONFIG_C5_TX_BOOT_GPIO) == 0;
        if (pressed != raw_pressed) {
            raw_pressed = pressed;
            raw_changed_us = now_us;
        }
        if (raw_pressed != stable_pressed && now_us - raw_changed_us >= BOOT_DEBOUNCE_US) {
            stable_pressed = raw_pressed;
            if (stable_pressed) transmit_button_profiles("BOOT");
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(MAIN_POLL_MS));
    }
#endif
}
