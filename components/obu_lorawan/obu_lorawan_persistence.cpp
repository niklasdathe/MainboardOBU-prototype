#include "obu_lorawan_persistence.hpp"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "obu_lwan_nvs";

namespace {

static constexpr char NVS_NAMESPACE[] = "obu_lwan";
static constexpr char NVS_KEY_PERSIST[] = "persist";
static constexpr char NVS_KEY_SESSION[] = "session";

struct persistence_context_t {
    nvs_handle_t handle = 0;
    LoRaWANNode *node = nullptr;
    bool prepared = false;
    bool attached = false;
    bool healthy = false;
    bool persistence_present = false;
    bool session_present = false;
    bool session_loaded = false;
    bool persistence_cache_valid = false;
    bool session_cache_valid = false;
    uint8_t persistence_cache[RADIOLIB_LORAWAN_PERSISTENCE_BUF_SIZE] = {};
    uint8_t session_cache[RADIOLIB_LORAWAN_SESSION_BUF_SIZE] = {};
};

persistence_context_t s_ctx;

void mark_unhealthy(const char *operation, esp_err_t err)
{
    s_ctx.healthy = false;
    ESP_LOGE(TAG,
             "LoRaWAN persistence failure during %s: %s; further LoRaWAN TX is blocked",
             operation,
             esp_err_to_name(err));
}

esp_err_t probe_blob(const char *key, size_t expected_len, bool *present)
{
    if (present == nullptr) return ESP_ERR_INVALID_ARG;
    *present = false;

    size_t stored_len = 0;
    const esp_err_t err = nvs_get_blob(s_ctx.handle, key, nullptr, &stored_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    if (stored_len != expected_len) {
        ESP_LOGE(TAG,
                 "LoRaWAN NVS blob '%s' has invalid length %u; expected %u",
                 key,
                 (unsigned)stored_len,
                 (unsigned)expected_len);
        return ESP_ERR_INVALID_SIZE;
    }

    *present = true;
    return ESP_OK;
}

void read_blob_or_fail(const char *key,
                       uint8_t *out,
                       size_t len,
                       uint8_t *cache,
                       bool *cache_valid)
{
    memset(out, 0, len);
    size_t stored_len = len;
    const esp_err_t err = nvs_get_blob(s_ctx.handle, key, out, &stored_len);
    if (err != ESP_OK || stored_len != len) {
        mark_unhealthy(key, err != ESP_OK ? err : ESP_ERR_INVALID_SIZE);
        return;
    }

    memcpy(cache, out, len);
    *cache_valid = true;
}

void write_blob_or_fail(const char *key,
                        uint8_t *data,
                        size_t len,
                        uint8_t *cache,
                        bool *cache_valid)
{
    if (!s_ctx.healthy) return;
    if (*cache_valid && memcmp(cache, data, len) == 0) return;

    esp_err_t err = nvs_set_blob(s_ctx.handle, key, data, len);
    if (err == ESP_OK) err = nvs_commit(s_ctx.handle);
    if (err != ESP_OK) {
        mark_unhealthy(key, err);
        return;
    }

    memcpy(cache, data, len);
    *cache_valid = true;
}

void store_persistence(uint8_t *buf, size_t len)
{
    if (len != RADIOLIB_LORAWAN_PERSISTENCE_BUF_SIZE) {
        mark_unhealthy("store persistence length", ESP_ERR_INVALID_SIZE);
        return;
    }
    write_blob_or_fail(NVS_KEY_PERSIST,
                       buf,
                       len,
                       s_ctx.persistence_cache,
                       &s_ctx.persistence_cache_valid);
    if (s_ctx.healthy) s_ctx.persistence_present = true;
}

void restore_persistence(uint8_t *buf, size_t len)
{
    if (len != RADIOLIB_LORAWAN_PERSISTENCE_BUF_SIZE) {
        memset(buf, 0, len);
        mark_unhealthy("restore persistence length", ESP_ERR_INVALID_SIZE);
        return;
    }
    read_blob_or_fail(NVS_KEY_PERSIST,
                      buf,
                      len,
                      s_ctx.persistence_cache,
                      &s_ctx.persistence_cache_valid);
}

void store_session(uint8_t *buf, size_t len)
{
    if (len != RADIOLIB_LORAWAN_SESSION_BUF_SIZE) {
        mark_unhealthy("store session length", ESP_ERR_INVALID_SIZE);
        return;
    }
    write_blob_or_fail(NVS_KEY_SESSION,
                       buf,
                       len,
                       s_ctx.session_cache,
                       &s_ctx.session_cache_valid);
    if (s_ctx.healthy) s_ctx.session_present = true;
}

void restore_session(uint8_t *buf, size_t len)
{
    if (len != RADIOLIB_LORAWAN_SESSION_BUF_SIZE) {
        memset(buf, 0, len);
        mark_unhealthy("restore session length", ESP_ERR_INVALID_SIZE);
        return;
    }
    read_blob_or_fail(NVS_KEY_SESSION,
                      buf,
                      len,
                      s_ctx.session_cache,
                      &s_ctx.session_cache_valid);
}

}  // namespace

esp_err_t obu_lorawan_persistence_prepare(void)
{
    if (s_ctx.prepared) return s_ctx.healthy ? ESP_OK : ESP_ERR_INVALID_STATE;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Unable to initialize default NVS partition: %s. Persistence is mandatory for LoRaWAN TX",
                 esp_err_to_name(err));
        return err;
    }

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &s_ctx.handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to open LoRaWAN NVS namespace: %s", esp_err_to_name(err));
        return err;
    }

    err = probe_blob(NVS_KEY_PERSIST,
                     RADIOLIB_LORAWAN_PERSISTENCE_BUF_SIZE,
                     &s_ctx.persistence_present);
    if (err == ESP_OK) {
        err = probe_blob(NVS_KEY_SESSION,
                         RADIOLIB_LORAWAN_SESSION_BUF_SIZE,
                         &s_ctx.session_present);
    }
    if (err != ESP_OK) {
        mark_unhealthy("NVS state probe", err);
        s_ctx.prepared = true;
        return err;
    }

    if (s_ctx.session_present && !s_ctx.persistence_present) {
        ESP_LOGE(TAG,
                 "LoRaWAN session blob exists without the mandatory nonce/persistence blob; refusing unsafe recovery");
        s_ctx.prepared = true;
        s_ctx.healthy = false;
        return ESP_ERR_INVALID_STATE;
    }

    s_ctx.prepared = true;
    s_ctx.healthy = true;
    ESP_LOGI(TAG,
             "LoRaWAN NVS ready: nonce_state=%s session_state=%s namespace=%s",
             s_ctx.persistence_present ? "present" : "fresh",
             s_ctx.session_present ? "present" : "fresh",
             NVS_NAMESPACE);
    return ESP_OK;
}

esp_err_t obu_lorawan_persistence_attach(LoRaWANNode *node,
                                         bool *restored_persistence,
                                         bool *restored_session)
{
    if (node == nullptr) return ESP_ERR_INVALID_ARG;
    if (restored_persistence != nullptr) *restored_persistence = false;
    if (restored_session != nullptr) *restored_session = false;

    esp_err_t err = obu_lorawan_persistence_prepare();
    if (err != ESP_OK) return err;

    if (s_ctx.attached) {
        if (s_ctx.node != node) return ESP_ERR_INVALID_STATE;
        if (restored_persistence != nullptr) *restored_persistence = s_ctx.persistence_present;
        if (restored_session != nullptr) *restored_session = s_ctx.session_loaded;
        return s_ctx.healthy ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    s_ctx.node = node;
    s_ctx.attached = true;

    node->setCallbackStorePersistence(store_persistence);
    node->setCallbackStoreSession(store_session);
    if (s_ctx.persistence_present) {
        node->setCallbackRestorePersistence(restore_persistence);
    }
    if (s_ctx.session_present) {
        node->setCallbackRestoreSession(restore_session);
    }

    if (s_ctx.persistence_present) {
        const int16_t state = node->loadBuffers();
        if (state != RADIOLIB_ERR_NONE || !s_ctx.healthy) {
            s_ctx.healthy = false;
            ESP_LOGE(TAG,
                     "RadioLib rejected saved LoRaWAN state (state=%d). Refusing to reset nonces automatically; explicitly erase/re-register the device before retrying",
                     (int)state);
            return ESP_ERR_INVALID_STATE;
        }
        s_ctx.session_loaded = s_ctx.session_present;
    }

    if (restored_persistence != nullptr) *restored_persistence = s_ctx.persistence_present;
    if (restored_session != nullptr) *restored_session = s_ctx.session_loaded;

    ESP_LOGI(TAG,
             "RadioLib persistence attached: nonce_state=%s session_restored=%s",
             s_ctx.persistence_present ? "restored" : "new",
             s_ctx.session_loaded ? "yes" : "no");
    return ESP_OK;
}

bool obu_lorawan_persistence_healthy(void)
{
    return s_ctx.prepared && s_ctx.attached && s_ctx.healthy;
}
