#include "obu_lorawan.h"

#include <ctype.h>
#include <new>
#include <string.h>

#include <RadioLib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "obu_lorawan";

static constexpr uint8_t FRAG_MAGIC_0 = 0x42;  // 'B'
static constexpr uint8_t FRAG_MAGIC_1 = 0x4f;  // 'O'
static constexpr size_t FRAG_HEADER_BYTES = OBU_LORAWAN_FRAGMENT_HEADER_BYTES;

typedef struct {
    uint16_t len;
    uint8_t data[OBU_LORAWAN_MAX_FRAME_BYTES_LIMIT];
} queued_frame_t;

struct obu_lorawan {
    obu_lorawan_config_t config = {};
    QueueHandle_t queue = nullptr;
    TaskHandle_t task = nullptr;
    EspHal *hal = nullptr;
    Module *module = nullptr;
    SX1262 *radio = nullptr;
    LoRaWANNode *node = nullptr;
    bool radio_ready = false;
    uint16_t frame_sequence = 0;
    obu_lorawan_stats_t stats = {};
    portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
};

static void stats_set_joined(obu_lorawan_t *u, bool joined)
{
    portENTER_CRITICAL(&u->stats_lock);
    u->stats.joined = joined;
    portEXIT_CRITICAL(&u->stats_lock);
}

static void stats_inc(uint32_t *counter, portMUX_TYPE *lock)
{
    portENTER_CRITICAL(lock);
    (*counter)++;
    portEXIT_CRITICAL(lock);
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    return -1;
}

static bool parse_hex_bytes(const char *text, uint8_t *out, size_t out_len)
{
    if (text == nullptr || out == nullptr || out_len == 0) return false;
    size_t nibbles = 0;
    int high = -1;

    memset(out, 0, out_len);
    for (const char *p = text; *p != '\0'; ++p) {
        int v = hex_nibble(*p);
        if (v < 0) {
            if (*p == ':' || *p == '-' || *p == ' ' || *p == '_') continue;
            return false;
        }
        if (high < 0) {
            high = v;
        } else {
            if ((nibbles / 2U) >= out_len) return false;
            out[nibbles / 2U] = (uint8_t)((high << 4) | v);
            high = -1;
        }
        nibbles++;
    }
    return high < 0 && nibbles == out_len * 2U;
}

static bool parse_eui(const char *text, uint64_t *out)
{
    uint8_t bytes[8];
    if (out == nullptr || !parse_hex_bytes(text, bytes, sizeof(bytes))) return false;

    uint64_t value = 0;
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        value = (value << 8U) | bytes[i];
    }
    *out = value;
    return true;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffffU;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8U;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000U) ? (uint16_t)((crc << 1U) ^ 0x1021U)
                                  : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

static void put_u16_be(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8U);
    dst[1] = (uint8_t)value;
}

static bool credentials_valid(const obu_lorawan_config_t *c,
                              uint64_t *join_eui,
                              uint64_t *dev_eui,
                              uint8_t nwk_key[16],
                              uint8_t app_key[16])
{
    return parse_eui(c->join_eui_hex, join_eui) &&
           parse_eui(c->dev_eui_hex, dev_eui) &&
           parse_hex_bytes(c->nwk_key_hex, nwk_key, 16) &&
           parse_hex_bytes(c->app_key_hex, app_key, 16);
}

static bool ensure_radio(obu_lorawan_t *u)
{
    if (u->radio_ready) return true;

    u->hal = new (std::nothrow) EspHal((int8_t)u->config.sck_gpio,
                                       (int8_t)u->config.miso_gpio,
                                       (int8_t)u->config.mosi_gpio,
                                       u->config.host,
                                       u->config.spi_clock_hz);
    if (u->hal == nullptr) return false;

    u->module = new (std::nothrow) Module(u->hal,
                                          (uint32_t)u->config.nss_gpio,
                                          (uint32_t)u->config.dio1_gpio,
                                          (uint32_t)u->config.reset_gpio,
                                          (uint32_t)u->config.busy_gpio);
    if (u->module == nullptr) return false;

    u->radio = new (std::nothrow) SX1262(u->module);
    if (u->radio == nullptr) return false;

    /*
     * Seeed Wio-SX1262 uses a TCXO driven from DIO3. The module datasheet
     * specifies software configuration of that rail; 1.8 V is the value used
     * by working XIAO/Wio-SX1262 board definitions.
     */
    int16_t state = u->radio->begin(868.0, 125.0, 9, 7,
                                    RADIOLIB_SX126X_SYNC_WORD_PRIVATE,
                                    10, 8, 1.8, false);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1262 initialization failed: RadioLib state=%d", (int)state);
        return false;
    }

    state = u->radio->setDio2AsRfSwitch(true);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "SX1262 DIO2 RF-switch setup failed: RadioLib state=%d", (int)state);
        return false;
    }

    u->node = new (std::nothrow) LoRaWANNode(u->radio, &EU868, 0);
    if (u->node == nullptr) return false;

    uint64_t join_eui = 0;
    uint64_t dev_eui = 0;
    uint8_t nwk_key[16];
    uint8_t app_key[16];
    if (!credentials_valid(&u->config, &join_eui, &dev_eui, nwk_key, app_key)) {
        ESP_LOGE(TAG,
                 "LoRaWAN credentials invalid: JoinEUI/DevEUI need 16 hex digits and both keys need 32");
        return false;
    }

    state = u->node->beginOTAA(join_eui, dev_eui, nwk_key, app_key);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "LoRaWAN OTAA configuration failed: RadioLib state=%d", (int)state);
        return false;
    }
    u->node->setADR(true);
    u->node->setDutyCycle(true);

    u->radio_ready = true;
    ESP_LOGI(TAG,
             "Wio-SX1262 ready on shared SPI%d: SCK=%d MISO=%d MOSI=%d NSS=%d DIO1=%d RESET=%d BUSY=%d",
             (int)u->config.host,
             u->config.sck_gpio, u->config.miso_gpio, u->config.mosi_gpio,
             u->config.nss_gpio, u->config.dio1_gpio,
             u->config.reset_gpio, u->config.busy_gpio);
    return true;
}

static bool ensure_joined(obu_lorawan_t *u)
{
    if (u->node == nullptr) return false;
    if (u->node->isActivated()) {
        stats_set_joined(u, true);
        return true;
    }

    stats_inc(&u->stats.join_attempts, &u->stats_lock);
    ESP_LOGI(TAG, "Joining LoRaWAN network using OTAA");
    int16_t state = u->node->activateOTAA();
    if (state == RADIOLIB_LORAWAN_NEW_SESSION ||
        state == RADIOLIB_LORAWAN_SESSION_RESTORED) {
        stats_set_joined(u, true);
        ESP_LOGI(TAG, "LoRaWAN OTAA active (RadioLib state=%d)", (int)state);
        return true;
    }

    stats_inc(&u->stats.join_failures, &u->stats_lock);
    stats_set_joined(u, false);
    ESP_LOGW(TAG, "LoRaWAN OTAA join failed: RadioLib state=%d", (int)state);
    return false;
}

static bool send_frame(obu_lorawan_t *u, const queued_frame_t *frame)
{
    const uint8_t data_bytes = u->config.fragment_data_bytes;
    const uint8_t fragment_count =
        (uint8_t)((frame->len + data_bytes - 1U) / data_bytes);
    const uint16_t crc = crc16_ccitt(frame->data, frame->len);
    const uint16_t frame_sequence = u->frame_sequence++;

    uint8_t payload[FRAG_HEADER_BYTES + OBU_LORAWAN_MAX_FRAGMENT_DATA_BYTES];
    for (uint8_t fragment_index = 0; fragment_index < fragment_count; ++fragment_index) {
        const size_t offset = (size_t)fragment_index * data_bytes;
        const size_t remaining = frame->len - offset;
        const size_t part_len = remaining < data_bytes ? remaining : data_bytes;

        payload[0] = FRAG_MAGIC_0;
        payload[1] = FRAG_MAGIC_1;
        payload[2] = OBU_LORAWAN_FRAGMENT_PROTOCOL_VERSION;
        payload[3] = 0;
        put_u16_be(payload + 4, frame_sequence);
        payload[6] = fragment_index;
        payload[7] = fragment_count;
        put_u16_be(payload + 8, frame->len);
        put_u16_be(payload + 10, crc);
        memcpy(payload + FRAG_HEADER_BYTES, frame->data + offset, part_len);

        int16_t state = u->node->sendReceive(payload,
                                              FRAG_HEADER_BYTES + part_len,
                                              u->config.fport,
                                              false);
        if (state < RADIOLIB_ERR_NONE) {
            stats_inc(&u->stats.tx_errors, &u->stats_lock);
            ESP_LOGW(TAG,
                     "LoRaWAN fragment TX failed: frame=%u fragment=%u/%u state=%d",
                     (unsigned)frame_sequence,
                     (unsigned)(fragment_index + 1U),
                     (unsigned)fragment_count,
                     (int)state);
            if (state == RADIOLIB_ERR_NETWORK_NOT_JOINED) {
                stats_set_joined(u, false);
            }
            return false;
        }

        stats_inc(&u->stats.fragments_sent, &u->stats_lock);
        ESP_LOGD(TAG,
                 "LoRaWAN fragment sent: frame=%u fragment=%u/%u bytes=%u fport=%u",
                 (unsigned)frame_sequence,
                 (unsigned)(fragment_index + 1U),
                 (unsigned)fragment_count,
                 (unsigned)(FRAG_HEADER_BYTES + part_len),
                 (unsigned)u->config.fport);

        if (fragment_index + 1U < fragment_count) {
            vTaskDelay(pdMS_TO_TICKS(u->config.min_fragment_interval_ms));
        }
    }

    stats_inc(&u->stats.frames_sent, &u->stats_lock);
    ESP_LOGI(TAG,
             "LoRaWAN raw frame sent: frame=%u bytes=%u fragments=%u",
             (unsigned)frame_sequence, (unsigned)frame->len, (unsigned)fragment_count);
    return true;
}

static void uplink_task(void *arg)
{
    auto *u = static_cast<obu_lorawan_t *>(arg);
    queued_frame_t frame = {};

    for (;;) {
        if (!u->radio_ready && !ensure_radio(u)) {
            ESP_LOGW(TAG, "LoRaWAN radio unavailable; retrying in %u ms",
                     (unsigned)u->config.join_retry_ms);
            vTaskDelay(pdMS_TO_TICKS(u->config.join_retry_ms));
            continue;
        }

        if (!ensure_joined(u)) {
            vTaskDelay(pdMS_TO_TICKS(u->config.join_retry_ms));
            continue;
        }

        if (xQueueReceive(u->queue, &frame, pdMS_TO_TICKS(1000)) != pdTRUE) continue;
        (void)send_frame(u, &frame);
        vTaskDelay(pdMS_TO_TICKS(u->config.min_fragment_interval_ms));
    }
}

extern "C" esp_err_t obu_lorawan_start(const obu_lorawan_config_t *config, obu_lorawan_t **out)
{
    if (config == nullptr || out == nullptr) return ESP_ERR_INVALID_ARG;
    if (config->max_frame_bytes == 0 ||
        config->max_frame_bytes > OBU_LORAWAN_MAX_FRAME_BYTES_LIMIT ||
        config->fragment_data_bytes == 0 ||
        config->fragment_data_bytes > OBU_LORAWAN_MAX_FRAGMENT_DATA_BYTES ||
        config->queue_depth == 0 ||
        config->fport == 0 ||
        config->fport > 223) {
        return ESP_ERR_INVALID_ARG;
    }

    auto *u = new (std::nothrow) obu_lorawan_t();
    if (u == nullptr) return ESP_ERR_NO_MEM;
    u->config = *config;
    *out = u;

    if (!config->enabled) {
        ESP_LOGI(TAG, "Wio-SX1262 LoRaWAN uplink disabled");
        return ESP_OK;
    }

    u->queue = xQueueCreate(config->queue_depth, sizeof(queued_frame_t));
    if (u->queue == nullptr) {
        delete u;
        *out = nullptr;
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(uplink_task, "obu_lorawan", 8192, u, 6, &u->task) != pdPASS) {
        vQueueDelete(u->queue);
        delete u;
        *out = nullptr;
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG,
             "LoRaWAN uplink worker started: EU868 fport=%u max_frame=%u fragment_data=%u queue=%u",
             (unsigned)config->fport,
             (unsigned)config->max_frame_bytes,
             (unsigned)config->fragment_data_bytes,
             (unsigned)config->queue_depth);
    return ESP_OK;
}

extern "C" esp_err_t obu_lorawan_enqueue_frame(obu_lorawan_t *u,
                                                 const uint8_t *frame,
                                                 size_t frame_len)
{
    if (u == nullptr || frame == nullptr || frame_len == 0) return ESP_ERR_INVALID_ARG;
    stats_inc(&u->stats.frames_offered, &u->stats_lock);

    if (!u->config.enabled || u->queue == nullptr) return ESP_ERR_NOT_SUPPORTED;
    if (frame_len > u->config.max_frame_bytes ||
        frame_len > OBU_LORAWAN_MAX_FRAME_BYTES_LIMIT) {
        stats_inc(&u->stats.frames_dropped_oversize, &u->stats_lock);
        return ESP_ERR_INVALID_SIZE;
    }

    queued_frame_t item = {};
    item.len = (uint16_t)frame_len;
    memcpy(item.data, frame, frame_len);

    if (xQueueSend(u->queue, &item, 0) != pdTRUE) {
        queued_frame_t discarded = {};
        if (xQueueReceive(u->queue, &discarded, 0) == pdTRUE) {
            stats_inc(&u->stats.frames_dropped_queue, &u->stats_lock);
        }
        if (xQueueSend(u->queue, &item, 0) != pdTRUE) {
            stats_inc(&u->stats.frames_dropped_queue, &u->stats_lock);
            return ESP_ERR_TIMEOUT;
        }
    }

    stats_inc(&u->stats.frames_queued, &u->stats_lock);
    return ESP_OK;
}

extern "C" void obu_lorawan_get_stats(const obu_lorawan_t *u, obu_lorawan_stats_t *out)
{
    if (out == nullptr) return;
    memset(out, 0, sizeof(*out));
    if (u == nullptr) return;

    auto *mutable_u = const_cast<obu_lorawan_t *>(u);
    portENTER_CRITICAL(&mutable_u->stats_lock);
    *out = mutable_u->stats;
    portEXIT_CRITICAL(&mutable_u->stats_lock);
}
