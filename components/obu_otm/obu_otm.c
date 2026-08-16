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

#define HOTSPOT_WIFI_RETRY_COUNT 5u
#define OTM_MQTT_NETWORK_TIMEOUT_MS 30000
#define OTM_MQTT_RECONNECT_TIMEOUT_MS 5000
#define OTM_MQTT_KEEPALIVE_S 30

/*
 * obu_otm is a shared component and is compiled for both the S3 and C5
 * firmware. The country-code Kconfig option belongs to the S3 application,
 * where the direct Wi-Fi uplink is actually used. Keep the shared component
 * buildable for targets which do not expose that application Kconfig symbol.
 */
#ifndef CONFIG_OBU_WIFI_COUNTRY_CODE
#define CONFIG_OBU_WIFI_COUNTRY_CODE "DE"
#endif

static const char *TAG = "obu_otm";

struct obu_otm {
    bool enabled;
    bool wifi_ready;
    bool mqtt_ready;
    bool mqtt_started;
    char topic[96];
    char status[96];
    char broker_uri[128];
    char wifi_ssid[33];
    esp_mqtt_client_handle_t mqtt;
    uint32_t attempted;
    uint32_t successful;
    uint32_t drops;
    uint32_t errors;
    uint32_t wifi_connect_attempts;
    uint32_t wifi_disconnects;
    uint32_t wifi_lost_ip_events;
    uint32_t wifi_no_ap_events;
    uint32_t mqtt_attempts;
    uint32_t mqtt_connects;
    uint32_t mqtt_errors;
    uint32_t mqtt_network_restarts;
};

static void log_wifi_link(void)
{
    wifi_ap_record_t ap = {0};
    const esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi link: RSSI=%d dBm channel=%u authmode=%u BSSID=%02x:%02x:%02x:%02x:%02x:%02x",
                 (int)ap.rssi,
                 (unsigned)ap.primary,
                 (unsigned)ap.authmode,
                 ap.bssid[0], ap.bssid[1], ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    } else {
        ESP_LOGW(TAG, "Unable to read Wi-Fi AP info: %s", esp_err_to_name(err));
    }
}

static void log_hotspot_compatibility_hint(struct obu_otm *o, uint8_t reason)
{
    if (o == NULL) return;

    if (reason == WIFI_REASON_NO_AP_FOUND) {
        o->wifi_no_ap_events++;
        if (o->wifi_no_ap_events == 1u || (o->wifi_no_ap_events % 5u) == 0u) {
            ESP_LOGW(TAG,
                     "Hotspot SSID '%s' was not found. The ESP32-S3 Wi-Fi radio is 2.4 GHz only; "
                     "the phone hotspot must expose a 2.4 GHz network. On iPhone 12 or newer use "
                     "Personal Hotspot -> Maximize Compatibility when the normal hotspot is not visible.",
                     o->wifi_ssid);
        }
    } else if (reason == WIFI_REASON_AUTH_FAIL || reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
        ESP_LOGW(TAG,
                 "Hotspot authentication/handshake failed (reason=%u). Open/OWE, WPA2-Personal, "
                 "WPA3-Personal and transition hotspots are supported; verify the configured hotspot password.",
                 (unsigned)reason);
    }
}

static void log_mqtt_error(esp_mqtt_event_handle_t event, struct obu_otm *o)
{
    o->mqtt_errors++;
    o->errors++;
    if (event == NULL || event->error_handle == NULL) {
        ESP_LOGE(TAG, "MQTT error #%u without error_handle", (unsigned)o->mqtt_errors);
        return;
    }

    const esp_mqtt_error_codes_t *err = event->error_handle;
    if (err->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
        ESP_LOGE(TAG,
                 "MQTT TCP/TLS error #%u: esp_tls=0x%x (%s) tls_stack=0x%x cert_flags=0x%x socket_errno=%d (%s)",
                 (unsigned)o->mqtt_errors,
                 (unsigned)err->esp_tls_last_esp_err,
                 esp_err_to_name(err->esp_tls_last_esp_err),
                 (unsigned)err->esp_tls_stack_err,
                 (unsigned)err->esp_tls_cert_verify_flags,
                 err->esp_transport_sock_errno,
                 err->esp_transport_sock_errno != 0 ? strerror(err->esp_transport_sock_errno) : "none");
    } else if (err->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
        ESP_LOGE(TAG, "MQTT broker refused connection #%u: return_code=0x%x",
                 (unsigned)o->mqtt_errors, (unsigned)err->connect_return_code);
    } else {
        ESP_LOGE(TAG, "MQTT error #%u: error_type=%u",
                 (unsigned)o->mqtt_errors, (unsigned)err->error_type);
    }
    ESP_LOGI(TAG,
             "MQTT diagnostic context: broker=%s wifi_ready=%s mqtt_started=%s attempts=%u connects=%u wifi_disconnects=%u lost_ip=%u network_restarts=%u",
             o->broker_uri,
             o->wifi_ready ? "yes" : "no",
             o->mqtt_started ? "yes" : "no",
             (unsigned)o->mqtt_attempts,
             (unsigned)o->mqtt_connects,
             (unsigned)o->wifi_disconnects,
             (unsigned)o->wifi_lost_ip_events,
             (unsigned)o->mqtt_network_restarts);
    log_wifi_link();
}

static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    struct obu_otm *o = arg;
    if (o == NULL) return;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;

    if (id == MQTT_EVENT_BEFORE_CONNECT) {
        o->mqtt_attempts++;
        ESP_LOGI(TAG, "OpenTrafficMap MQTT attempting connection #%u to %s",
                 (unsigned)o->mqtt_attempts, o->broker_uri);
    } else if (id == MQTT_EVENT_CONNECTED) {
        o->mqtt_ready = true;
        o->mqtt_connects++;
        ESP_LOGI(TAG, "OpenTrafficMap MQTT connected (connection #%u after %u attempts)",
                 (unsigned)o->mqtt_connects, (unsigned)o->mqtt_attempts);
        const int msg_id = esp_mqtt_client_publish(o->mqtt, o->status, "online", 0, 0, 1);
        ESP_LOGI(TAG, "Published OTM online status: topic=%s msg_id=%d", o->status, msg_id);
    } else if (id == MQTT_EVENT_DISCONNECTED) {
        o->mqtt_ready = false;
        if (o->wifi_ready && o->mqtt_started) {
            ESP_LOGW(TAG, "OpenTrafficMap MQTT disconnected; client will retry while Wi-Fi remains available");
        } else {
            ESP_LOGI(TAG, "OpenTrafficMap MQTT disconnected because network/client is stopping");
        }
    } else if (id == MQTT_EVENT_ERROR) {
        o->mqtt_ready = false;
        log_mqtt_error(event, o);
    } else if (id == MQTT_EVENT_PUBLISHED && event != NULL) {
        ESP_LOGD(TAG, "MQTT publish acknowledged msg_id=%d", event->msg_id);
    } else {
        ESP_LOGD(TAG, "MQTT event id=%ld", (long)id);
    }
}

static void stop_mqtt_for_network_loss(struct obu_otm *o)
{
    if (o == NULL || o->mqtt == NULL || !o->mqtt_started) return;

    /* Throw away the old TCP/TLS state whenever tethering changes underneath
     * us. Phone hotspots frequently renumber/re-NAT clients after radio or
     * cellular transitions, so reusing the old socket is less reliable than a
     * clean MQTT restart after the next DHCP lease. */
    ESP_LOGI(TAG, "Stopping OpenTrafficMap MQTT because Wi-Fi/IP is unavailable");
    const esp_err_t err = esp_mqtt_client_stop(o->mqtt);
    if (err == ESP_OK) {
        o->mqtt_network_restarts++;
    } else {
        ESP_LOGW(TAG, "Stopping OpenTrafficMap MQTT returned %s", esp_err_to_name(err));
    }
    o->mqtt_started = false;
    o->mqtt_ready = false;
}

static void start_mqtt_after_ip(struct obu_otm *o)
{
    if (o == NULL || o->mqtt == NULL || o->mqtt_started || !o->wifi_ready) return;

    ESP_LOGI(TAG, "Wi-Fi has IP; starting OpenTrafficMap MQTT client for %s", o->broker_uri);
    const esp_err_t err = esp_mqtt_client_start(o->mqtt);
    if (err == ESP_OK) {
        o->mqtt_started = true;
    } else {
        o->errors++;
        ESP_LOGE(TAG, "Failed to start OpenTrafficMap MQTT client: %s", esp_err_to_name(err));
    }
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct obu_otm *o = arg;
    if (o == NULL) return;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        o->wifi_connect_attempts++;
        ESP_LOGI(TAG, "Wi-Fi STA started; connecting to SSID '%s' (attempt #%u)",
                 o->wifi_ssid, (unsigned)o->wifi_connect_attempts);
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(err));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        o->wifi_no_ap_events = 0;
        ESP_LOGI(TAG, "Wi-Fi associated with SSID '%s'; waiting for DHCP/IP", o->wifi_ssid);
        log_wifi_link();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        o->wifi_ready = false;
        o->wifi_disconnects++;
        stop_mqtt_for_network_loss(o);

        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        if (disc != NULL) {
            ESP_LOGW(TAG, "Wi-Fi disconnected #%u: reason=%u RSSI=%d; reconnecting",
                     (unsigned)o->wifi_disconnects, (unsigned)disc->reason, (int)disc->rssi);
            log_hotspot_compatibility_hint(o, disc->reason);
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected #%u; reconnecting", (unsigned)o->wifi_disconnects);
        }
        o->wifi_connect_attempts++;
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(err));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        o->wifi_ready = false;
        o->wifi_lost_ip_events++;
        ESP_LOGW(TAG, "Wi-Fi lost IP lease #%u; waiting for a fresh DHCP lease before MQTT restart",
                 (unsigned)o->wifi_lost_ip_events);
        stop_mqtt_for_network_loss(o);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        o->wifi_ready = true;
        const ip_event_got_ip_t *got = (const ip_event_got_ip_t *)data;
        if (got != NULL) {
            ESP_LOGI(TAG, "Wi-Fi acquired IP: " IPSTR " netmask=" IPSTR " gateway=" IPSTR " changed=%s",
                     IP2STR(&got->ip_info.ip),
                     IP2STR(&got->ip_info.netmask),
                     IP2STR(&got->ip_info.gw),
                     got->ip_changed ? "yes" : "no");
        } else {
            ESP_LOGI(TAG, "Wi-Fi acquired IP address");
        }
        log_wifi_link();
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
        ESP_LOGI(TAG, "Direct OpenTrafficMap Wi-Fi uploader disabled");
        *out = o;
        return ESP_OK;
    }
    if (c->wifi_ssid == NULL || c->wifi_ssid[0] == '\0' || c->node_id == NULL || c->node_id[0] == '\0') {
        ESP_LOGE(TAG, "OTM uploader enabled but SSID or node ID is empty");
        free(o);
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(o->topic, sizeof(o->topic), "its/%s/packet", c->node_id);
    snprintf(o->status, sizeof(o->status), "its/%s/status", c->node_id);
    snprintf(o->broker_uri, sizeof(o->broker_uri), "%s",
             c->broker_uri != NULL ? c->broker_uri : "mqtts://cits1.opentrafficmap.org");
    snprintf(o->wifi_ssid, sizeof(o->wifi_ssid), "%s", c->wifi_ssid);

    ESP_LOGI(TAG, "OTM direct uploader config: SSID='%s' broker='%s' node='%s' packet_topic='%s'",
             o->wifi_ssid, o->broker_uri, c->node_id, o->topic);
    ESP_LOGI(TAG,
             "Hotspot compatibility mode: 2.4GHz b/g/n, country=%s with 802.11d auto-update, all-channel scan, "
             "Open/OWE + WPA2/WPA3 Personal, PMF capable, Wi-Fi power-save disabled, MQTT network timeout=%dms",
             CONFIG_OBU_WIFI_COUNTRY_CODE, OTM_MQTT_NETWORK_TIMEOUT_MS);
#ifdef CONFIG_OBU_OTM_TLS_TRACE
    ESP_LOGW(TAG, "OpenTrafficMap TLS trace enabled; mbedTLS handshake diagnostics are active for development");
#endif

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS needs erase/reinitialize: %s", esp_err_to_name(err));
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

    /* The default world-safe domain only scans channels 1-11 before association.
     * Configure the actual operating country so European phone hotspots on
     * channels 12/13 can be discovered. 802.11d lets the AP update the country
     * information after association. */
    err = esp_wifi_set_country_code(CONFIG_OBU_WIFI_COUNTRY_CODE, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invalid/unsupported Wi-Fi country code '%s': %s",
                 CONFIG_OBU_WIFI_COUNTRY_CODE, esp_err_to_name(err));
        return err;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid) - 1);
    const bool secured_hotspot = c->wifi_password != NULL && c->wifi_password[0] != '\0';
    if (secured_hotspot) strncpy((char *)wc.sta.password, c->wifi_password, sizeof(wc.sta.password) - 1);

    /* Avoid assumptions about a phone vendor's selected 2.4 GHz channel or
     * WPA2/WPA3/OWE transition behavior. WPA2 is the minimum for password-
     * protected hotspots; passwordless profiles also allow OWE/OWE transition. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.channel = 0;
    wc.sta.bssid_set = false;
    wc.sta.threshold.rssi = -127;
    wc.sta.threshold.authmode = secured_hotspot ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    wc.sta.owe_enabled = !secured_hotspot;
    wc.sta.failure_retry_cnt = HOTSPOT_WIFI_RETRY_COUNT;

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, o);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_evt, o);
    if (err != ESP_OK) return err;

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = o->broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .network.timeout_ms = OTM_MQTT_NETWORK_TIMEOUT_MS,
        .network.reconnect_timeout_ms = OTM_MQTT_RECONNECT_TIMEOUT_MS,
        .session.keepalive = OTM_MQTT_KEEPALIVE_S,
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

    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    /* Phone tethering is a development uplink where reliability matters more
     * than S3 Wi-Fi energy savings. Keeping the radio awake also removes modem
     * sleep latency from TLS handshakes and short hotspot beacon/DTIM windows. */
    const esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to disable Wi-Fi power save: %s", esp_err_to_name(ps_err));
    } else {
        ESP_LOGI(TAG, "Wi-Fi modem power save disabled for hotspot reliability");
    }

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
        if (o->drops == 1u || (o->drops % 100u) == 0u) {
            ESP_LOGW(TAG, "Dropping OTM frame while MQTT disconnected: dropped=%u len=%u",
                     (unsigned)o->drops, (unsigned)n);
        }
        return ESP_ERR_INVALID_STATE;
    }

    int id = esp_mqtt_client_publish(o->mqtt, o->topic, (const char *)f, (int)n, 0, 0);
    if (id < 0) {
        o->errors++;
        ESP_LOGE(TAG, "OTM publish failed: len=%u attempted=%u errors=%u",
                 (unsigned)n, (unsigned)o->attempted, (unsigned)o->errors);
        return ESP_FAIL;
    }
    o->successful++;
    if (o->successful == 1u || (o->successful % 100u) == 0u) {
        ESP_LOGI(TAG, "OTM frames published=%u attempted=%u last_len=%u msg_id=%d",
                 (unsigned)o->successful, (unsigned)o->attempted, (unsigned)n, id);
    }
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