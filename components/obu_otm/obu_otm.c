#include "obu_otm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

static const char *TAG = "obu_otm";

struct obu_otm {
    bool enabled;
    bool wifi_ready;
    bool mqtt_ready;
    bool mqtt_started;
    char topic[96];
    char status[96];
    esp_mqtt_client_handle_t mqtt;
    uint32_t attempted;
    uint32_t successful;
    uint32_t drops;
    uint32_t errors;
};

static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    (void)data;
    struct obu_otm *o = arg;
    if (o == NULL) return;

    if (id == MQTT_EVENT_CONNECTED) {
        o->mqtt_ready = true;
        ESP_LOGI(TAG, "OpenTrafficMap MQTT connected");
        (void)esp_mqtt_client_publish(o->mqtt, o->status, "online", 0, 0, 1);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        o->mqtt_ready = false;
        ESP_LOGW(TAG, "OpenTrafficMap MQTT disconnected");
    } else if (id == MQTT_EVENT_ERROR) {
        o->mqtt_ready = false;
        ESP_LOGW(TAG, "OpenTrafficMap MQTT transport error");
    }
}

static void start_mqtt_after_ip(struct obu_otm *o)
{
    if (o == NULL || o->mqtt == NULL || o->mqtt_started || !o->wifi_ready) return;

    const esp_err_t err = esp_mqtt_client_start(o->mqtt);
    if (err == ESP_OK) {
        o->mqtt_started = true;
        ESP_LOGI(TAG, "Wi-Fi has IP; starting OpenTrafficMap MQTT client");
    } else {
        o->errors++;
        ESP_LOGE(TAG, "Failed to start OpenTrafficMap MQTT client: %s", esp_err_to_name(err));
    }
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)data;
    struct obu_otm *o = arg;
    if (o == NULL) return;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        o->wifi_ready = false;
        o->mqtt_ready = false;
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(err));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        o->wifi_ready = true;
        ESP_LOGI(TAG, "Wi-Fi acquired IP address");
        start_mqtt_after_ip(o);
    }
}

esp_err_t obu_otm_wifi_start(const obu_otm_wifi_config_t *c, obu_otm_t **out)
{
    if (c == NULL || out == NULL) return ESP_ERR_INVALID_ARG;

    struct obu_otm *o = calloc(1, sizeof(*o));
    if (o == NULL) return ESP_ERR_NO_MEM;
    o->enabled = c->enabled;

    if (!c->enabled) {
        *out = o;
        return ESP_OK;
    }
    if (c->wifi_ssid == NULL || c->wifi_ssid[0] == '\0' ||
        c->node_id == NULL || c->node_id[0] == '\0') {
        free(o);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(o->topic, sizeof(o->topic), "its/%s/packet", c->node_id);
    snprintf(o->status, sizeof(o->status), "its/%s/status", c->node_id);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        err = nvs_flash_erase();
        if (err != ESP_OK) {
            free(o);
            return err;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        free(o);
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        free(o);
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        free(o);
        return err;
    }

    if (esp_netif_create_default_wifi_sta() == NULL) {
        free(o);
        return ESP_FAIL;
    }

    wifi_init_config_t wi = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wi);
    if (err != ESP_OK) {
        free(o);
        return err;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid) - 1);
    if (c->wifi_password != NULL) {
        strncpy((char *)wc.sta.password, c->wifi_password, sizeof(wc.sta.password) - 1);
    }

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, o);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, o);
    if (err != ESP_OK) return err;

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = c->broker_uri != NULL ? c->broker_uri : "mqtts://cits1.opentrafficmap.org:8883",
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .session.last_will.topic = o->status,
        .session.last_will.msg = "offline",
        .session.last_will.retain = 1,
        .session.last_will.qos = 0,
    };
    o->mqtt = esp_mqtt_client_init(&mc);
    if (o->mqtt == NULL) return ESP_FAIL;

    err = esp_mqtt_client_register_event(o->mqtt, ESP_EVENT_ANY_ID, mqtt_evt, o);
    if (err != ESP_OK) return err;

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) return err;

    /*
     * Start Wi-Fi last. MQTT is deliberately not started here: DNS/TLS needs
     * a valid IP route. The IP_EVENT_STA_GOT_IP handler starts MQTT once the
     * station has actually acquired an address.
     */
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    *out = o;
    return ESP_OK;
}

esp_err_t obu_otm_publish_live_frame(obu_otm_t *o, const uint8_t *f, size_t n)
{
    if (o == NULL || f == NULL || n == 0) return ESP_ERR_INVALID_ARG;
    o->attempted++;
    if (!o->enabled) {
        o->errors++;
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!o->mqtt_ready) {
        o->drops++;
        o->errors++;
        return ESP_ERR_INVALID_STATE;
    }

    int id = esp_mqtt_client_publish(o->mqtt, o->topic, (const char *)f, (int)n, 0, 0);
    if (id < 0) {
        o->errors++;
        return ESP_FAIL;
    }
    o->successful++;
    return ESP_OK;
}

uint32_t obu_otm_drop_disconnected(const obu_otm_t *o)
{
    return o != NULL ? o->drops : 0;
}

uint32_t obu_otm_publish_errors(const obu_otm_t *o)
{
    return o != NULL ? o->errors : 0;
}

void obu_otm_get_stats(const obu_otm_t *o, obu_otm_stats_t *s)
{
    if (s == NULL) return;
    memset(s, 0, sizeof(*s));
    if (o != NULL) {
        *s = (obu_otm_stats_t){
            .attempted = o->attempted,
            .successful = o->successful,
            .failed = o->errors,
            .dropped_disconnected = o->drops,
            .connected = o->mqtt_ready,
        };
    }
}
