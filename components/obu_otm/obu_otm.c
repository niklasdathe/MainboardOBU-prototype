#include "obu_otm.h"
#include "obu_doh.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

static const char *TAG = "obu_otm";

#define OBU_DNS_RETRY_DELAY_MS 3000U
#define OBU_DNS_TASK_STACK_SIZE 4096U
#define OBU_DNS_TASK_PRIORITY 5U

struct obu_otm {
    bool enabled;
    bool wifi_ready;
    bool mqtt_ready;
    bool mqtt_started;
    bool dns_main_promoted;
    volatile bool dns_task_running;
    char topic[96];
    char status[96];
    char broker_uri[128];
    char broker_host[96];
    char broker_ip[16];
    char broker_ip_uri[64];
    char wifi_ssid[33];
    uint16_t broker_port;
    esp_netif_t *sta_netif;
    esp_mqtt_client_handle_t mqtt;
    uint32_t attempted;
    uint32_t successful;
    uint32_t drops;
    uint32_t errors;
    uint32_t wifi_connect_attempts;
    uint32_t wifi_disconnects;
    uint32_t dns_attempts;
    uint32_t dns_failures;
    uint32_t dns_promotions;
    uint32_t doh_attempts;
    uint32_t doh_failures;
    uint32_t mqtt_attempts;
    uint32_t mqtt_connects;
    uint32_t mqtt_errors;
    uint32_t mqtt_network_restarts;
};

static bool wifi_debug_enabled(void)
{
#ifdef CONFIG_OBU_WIFI_DEBUG
    return true;
#else
    return false;
#endif
}

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        default: return "OTHER";
    }
}

static bool broker_hostname_from_uri(const char *uri, char *host, size_t host_size)
{
    if (uri == NULL || host == NULL || host_size < 2U) return false;

    const char *authority = strstr(uri, "://");
    authority = authority != NULL ? authority + 3 : uri;
    if (*authority == '\0') return false;

    const char *authority_end = strchr(authority, '/');
    if (authority_end == NULL) authority_end = authority + strlen(authority);

    const char *host_start = authority;
    for (const char *p = authority; p < authority_end; ++p) {
        if (*p == '@') host_start = p + 1;
    }
    if (host_start >= authority_end) return false;

    const char *host_end = authority_end;
    if (*host_start == '[') {
        ++host_start;
        host_end = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (host_end == NULL) return false;
    } else {
        const char *colon = memchr(host_start, ':', (size_t)(authority_end - host_start));
        if (colon != NULL) host_end = colon;
    }

    const size_t len = (size_t)(host_end - host_start);
    if (len == 0U || len >= host_size) return false;
    memcpy(host, host_start, len);
    host[len] = '\0';
    return true;
}

static uint16_t broker_port_from_uri(const char *uri)
{
    if (uri == NULL) return 0U;
    const bool secure = strncmp(uri, "mqtts://", 8U) == 0;
    const uint16_t default_port = secure ? 8883U : 1883U;

    const char *authority = strstr(uri, "://");
    authority = authority != NULL ? authority + 3 : uri;
    const char *authority_end = strchr(authority, '/');
    if (authority_end == NULL) authority_end = authority + strlen(authority);

    const char *host_start = authority;
    for (const char *p = authority; p < authority_end; ++p) {
        if (*p == '@') host_start = p + 1;
    }
    if (host_start >= authority_end) return default_port;

    const char *port_start = NULL;
    if (*host_start == '[') {
        const char *closing = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (closing != NULL && closing + 1 < authority_end && closing[1] == ':') port_start = closing + 2;
    } else {
        const char *colon = memchr(host_start, ':', (size_t)(authority_end - host_start));
        if (colon != NULL) port_start = colon + 1;
    }

    if (port_start == NULL || port_start >= authority_end) return default_port;
    char port_text[6] = {0};
    const size_t port_len = (size_t)(authority_end - port_start);
    if (port_len == 0U || port_len >= sizeof(port_text)) return default_port;
    memcpy(port_text, port_start, port_len);

    char *end = NULL;
    const unsigned long parsed = strtoul(port_text, &end, 10);
    if (end == port_text || *end != '\0' || parsed == 0UL || parsed > 65535UL) return default_port;
    return (uint16_t)parsed;
}

static void enable_wifi_debug_logging(void)
{
    if (!wifi_debug_enabled()) return;

    esp_log_level_set(TAG, ESP_LOG_DEBUG);
    esp_log_level_set("wifi", ESP_LOG_DEBUG);
    esp_log_level_set("wifi_init", ESP_LOG_DEBUG);
    esp_log_level_set("esp_netif_handlers", ESP_LOG_DEBUG);

    ESP_LOGW(TAG,
             "Wi-Fi debug diagnostics enabled: driver/netif DEBUG logs and OBU connection-state diagnostics are active; password contents are never logged");
}

static void log_wifi_config(const char *stage)
{
    if (!wifi_debug_enabled()) return;

    wifi_config_t config = {0};
    const esp_err_t config_err = esp_wifi_get_config(WIFI_IF_STA, &config);
    if (config_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi debug [%s]: unable to read STA config: %s",
                 stage != NULL ? stage : "state", esp_err_to_name(config_err));
        return;
    }

    wifi_ps_type_t power_save = WIFI_PS_MIN_MODEM;
    const esp_err_t ps_err = esp_wifi_get_ps(&power_save);
    if (ps_err != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi debug [%s]: unable to read power-save mode: %s",
                 stage != NULL ? stage : "state", esp_err_to_name(ps_err));
    }

    ESP_LOGI(TAG,
             "Wi-Fi debug [%s]: SSID='%s' password_len=%u scan_method=%u sort_method=%u fixed_channel=%u threshold_rssi=%d threshold_auth=%u PMF(capable=%u required=%u) power_save=%d",
             stage != NULL ? stage : "state",
             (const char *)config.sta.ssid,
             (unsigned)strnlen((const char *)config.sta.password, sizeof(config.sta.password)),
             (unsigned)config.sta.scan_method,
             (unsigned)config.sta.sort_method,
             (unsigned)config.sta.channel,
             (int)config.sta.threshold.rssi,
             (unsigned)config.sta.threshold.authmode,
             (unsigned)config.sta.pmf_cfg.capable,
             (unsigned)config.sta.pmf_cfg.required,
             ps_err == ESP_OK ? (int)power_save : -1);
}

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

static void log_dns_server(esp_netif_t *netif, esp_netif_dns_type_t type, const char *name, const char *stage)
{
    esp_netif_dns_info_t dns = {0};
    const esp_err_t err = esp_netif_get_dns_info(netif, type, &dns);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "DNS [%s] %s: unavailable (%s)", stage, name, esp_err_to_name(err));
        return;
    }

    if (dns.ip.type == IPADDR_TYPE_V4) {
        char address[16] = {0};
        if (esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, address, sizeof(address)) != NULL) {
            ESP_LOGI(TAG, "DNS [%s] %s=%s", stage, name, address);
            return;
        }
    }

    ESP_LOGI(TAG, "DNS [%s] %s: configured address type=%u",
             stage, name, (unsigned)dns.ip.type);
}

static void log_dns_config(esp_netif_t *netif, const char *stage)
{
    if (netif == NULL) {
        ESP_LOGW(TAG, "DNS [%s]: STA netif unavailable", stage);
        return;
    }

    log_dns_server(netif, ESP_NETIF_DNS_MAIN, "main", stage);
    log_dns_server(netif, ESP_NETIF_DNS_BACKUP, "backup", stage);
    log_dns_server(netif, ESP_NETIF_DNS_FALLBACK, "fallback", stage);
}

static void configure_dns_fallback(esp_netif_t *netif)
{
#ifdef CONFIG_OBU_DNS_FALLBACK_ENABLE
    if (netif == NULL) return;

    esp_ip4_addr_t fallback_ip = {0};
    const esp_err_t parse_err = esp_netif_str_to_ip4(CONFIG_OBU_DNS_FALLBACK_IPV4, &fallback_ip);
    if (parse_err != ESP_OK) {
        ESP_LOGE(TAG, "Configured fallback DNS '%s' is not a valid IPv4 address",
                 CONFIG_OBU_DNS_FALLBACK_IPV4);
        return;
    }

    esp_netif_dns_info_t dns = {0};
    dns.ip.type = IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = fallback_ip.addr;

    const esp_err_t err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_FALLBACK, &dns);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Installed configurable DNS fallback %s", CONFIG_OBU_DNS_FALLBACK_IPV4);
    } else {
        ESP_LOGW(TAG, "Unable to install DNS fallback %s: %s",
                 CONFIG_OBU_DNS_FALLBACK_IPV4, esp_err_to_name(err));
    }
#else
    (void)netif;
#endif
}

static bool promote_dns_fallback_to_main(struct obu_otm *o)
{
#ifdef CONFIG_OBU_DNS_FALLBACK_ENABLE
    if (o == NULL || o->sta_netif == NULL || o->dns_main_promoted) return false;

    esp_netif_dns_info_t fallback = {0};
    esp_err_t err = esp_netif_get_dns_info(o->sta_netif, ESP_NETIF_DNS_FALLBACK, &fallback);
    if (err != ESP_OK || fallback.ip.type != IPADDR_TYPE_V4 || fallback.ip.u_addr.ip4.addr == 0U) {
        ESP_LOGW(TAG, "Cannot promote fallback DNS: configured fallback is unavailable");
        return false;
    }

    esp_netif_dns_info_t current_main = {0};
    const esp_err_t get_main_err = esp_netif_get_dns_info(o->sta_netif, ESP_NETIF_DNS_MAIN, &current_main);
    if (get_main_err == ESP_OK &&
        current_main.ip.type == IPADDR_TYPE_V4 &&
        current_main.ip.u_addr.ip4.addr != 0U &&
        current_main.ip.u_addr.ip4.addr != fallback.ip.u_addr.ip4.addr) {
        const esp_err_t backup_err = esp_netif_set_dns_info(o->sta_netif, ESP_NETIF_DNS_BACKUP, &current_main);
        if (backup_err != ESP_OK) {
            ESP_LOGW(TAG, "Unable to preserve DHCP DNS as backup before failover: %s",
                     esp_err_to_name(backup_err));
        }
    }

    err = esp_netif_set_dns_info(o->sta_netif, ESP_NETIF_DNS_MAIN, &fallback);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Unable to promote fallback DNS to main resolver: %s", esp_err_to_name(err));
        return false;
    }

    o->dns_main_promoted = true;
    o->dns_promotions++;
    ESP_LOGW(TAG,
             "DNS failover #%u: promoted %s to MAIN after resolver timeout; prior DHCP DNS retained as BACKUP when available",
             (unsigned)o->dns_promotions,
             CONFIG_OBU_DNS_FALLBACK_IPV4);
    log_dns_config(o->sta_netif, "FAILOVER");
    return true;
#else
    (void)o;
    return false;
#endif
}

static bool configure_mqtt_resolved_ip(struct obu_otm *o, const char *ipv4)
{
    if (o == NULL || o->mqtt == NULL || ipv4 == NULL || ipv4[0] == '\0') return false;

    const bool secure = strncmp(o->broker_uri, "mqtts://", 8U) == 0;
    snprintf(o->broker_ip, sizeof(o->broker_ip), "%s", ipv4);
    const int written = snprintf(o->broker_ip_uri,
                                 sizeof(o->broker_ip_uri),
                                 "%s://%s:%u",
                                 secure ? "mqtts" : "mqtt",
                                 o->broker_ip,
                                 (unsigned)o->broker_port);
    if (written <= 0 || (size_t)written >= sizeof(o->broker_ip_uri)) {
        ESP_LOGE(TAG, "Resolved broker URI does not fit for IPv4 '%s'", ipv4);
        return false;
    }

    const esp_err_t err = esp_mqtt_client_set_uri(o->mqtt, o->broker_ip_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Unable to configure MQTT resolved-IP target %s: %s",
                 o->broker_ip_uri, esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG,
             "MQTT target pinned to resolved IPv4 %s:%u; TLS identity/SNI remains '%s'",
             o->broker_ip,
             (unsigned)o->broker_port,
             o->broker_host);
    return true;
}

static bool resolve_broker_hostname(struct obu_otm *o)
{
    if (o == NULL || o->broker_host[0] == '\0') return false;

    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    o->dns_attempts++;
    const int rc = getaddrinfo(o->broker_host, NULL, &hints, &result);
    if (rc != 0 || result == NULL) {
        o->dns_failures++;
        if (result != NULL) freeaddrinfo(result);
        ESP_LOGW(TAG,
                 "DNS preflight #%u failed for '%s': getaddrinfo=%d failures=%u; MQTT start deferred",
                 (unsigned)o->dns_attempts,
                 o->broker_host,
                 rc,
                 (unsigned)o->dns_failures);
        return false;
    }

    char resolved_ipv4[16] = {0};
    const struct sockaddr_in *addr = (const struct sockaddr_in *)result->ai_addr;
    const char *ntop = inet_ntop(AF_INET, &addr->sin_addr, resolved_ipv4, sizeof(resolved_ipv4));
    if (ntop == NULL || !configure_mqtt_resolved_ip(o, resolved_ipv4)) {
        freeaddrinfo(result);
        o->dns_failures++;
        ESP_LOGW(TAG, "DNS preflight #%u resolved '%s' but could not configure MQTT target",
                 (unsigned)o->dns_attempts, o->broker_host);
        return false;
    }

    ESP_LOGI(TAG, "DNS preflight #%u resolved '%s' -> %s; MQTT may start",
             (unsigned)o->dns_attempts, o->broker_host, resolved_ipv4);
    freeaddrinfo(result);
    return true;
}

static bool resolve_broker_doh(struct obu_otm *o)
{
    if (o == NULL) return false;

    char resolved_ipv4[16] = {0};
    o->doh_attempts++;
    ESP_LOGW(TAG,
             "Classic DNS attempts failed; starting DoH fallback #%u for '%s' over HTTPS/443",
             (unsigned)o->doh_attempts,
             o->broker_host);
    const esp_err_t err = obu_doh_resolve_ipv4(o->broker_host, resolved_ipv4, sizeof(resolved_ipv4));
    if (err != ESP_OK) {
        o->doh_failures++;
        ESP_LOGW(TAG, "DoH fallback #%u failed for '%s': %s failures=%u",
                 (unsigned)o->doh_attempts,
                 o->broker_host,
                 esp_err_to_name(err),
                 (unsigned)o->doh_failures);
        return false;
    }

    if (!configure_mqtt_resolved_ip(o, resolved_ipv4)) {
        o->doh_failures++;
        return false;
    }

    ESP_LOGI(TAG, "DoH fallback #%u resolved broker '%s' -> %s",
             (unsigned)o->doh_attempts, o->broker_host, resolved_ipv4);
    return true;
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
             "MQTT diagnostic context: broker=%s resolved_ip=%s wifi_ready=%s mqtt_started=%s dns_attempts=%u dns_failures=%u doh_attempts=%u doh_failures=%u attempts=%u connects=%u wifi_disconnects=%u network_restarts=%u",
             o->broker_uri,
             o->broker_ip[0] != '\0' ? o->broker_ip : "unresolved",
             o->wifi_ready ? "yes" : "no",
             o->mqtt_started ? "yes" : "no",
             (unsigned)o->dns_attempts,
             (unsigned)o->dns_failures,
             (unsigned)o->doh_attempts,
             (unsigned)o->doh_failures,
             (unsigned)o->mqtt_attempts,
             (unsigned)o->mqtt_connects,
             (unsigned)o->wifi_disconnects,
             (unsigned)o->mqtt_network_restarts);
    log_wifi_link();
    log_dns_config(o->sta_netif, "MQTT_ERROR");
}

static void mqtt_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)base;
    struct obu_otm *o = arg;
    if (o == NULL) return;
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)data;

    if (id == MQTT_EVENT_BEFORE_CONNECT) {
        o->mqtt_attempts++;
        ESP_LOGI(TAG, "OpenTrafficMap MQTT attempting connection #%u to %s (identity=%s)",
                 (unsigned)o->mqtt_attempts,
                 o->broker_ip_uri[0] != '\0' ? o->broker_ip_uri : o->broker_uri,
                 o->broker_host);
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

    /* This function is called from the Wi-Fi event handler, not from an MQTT
     * callback. Stop the MQTT task/socket explicitly so the next DHCP lease
     * starts from a fresh TCP/TLS state, matching the OpenTrafficMap receiver
     * lifecycle of MQTT-up on GOT_IP and MQTT-down on network loss. */
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
    if (o->broker_ip[0] == '\0') {
        ESP_LOGW(TAG, "Refusing to start MQTT before broker IPv4 resolution is complete");
        return;
    }

    ESP_LOGI(TAG, "DNS is ready; starting OpenTrafficMap MQTT client for %s via %s",
             o->broker_uri, o->broker_ip_uri);
    const esp_err_t err = esp_mqtt_client_start(o->mqtt);
    if (err == ESP_OK) {
        o->mqtt_started = true;
    } else {
        o->errors++;
        ESP_LOGE(TAG, "Failed to start OpenTrafficMap MQTT client: %s", esp_err_to_name(err));
    }
}

static void dns_gate_task(void *arg)
{
    struct obu_otm *o = arg;
    if (o == NULL) {
        vTaskDelete(NULL);
        return;
    }

    log_dns_config(o->sta_netif, "DHCP");
    configure_dns_fallback(o->sta_netif);
    log_dns_config(o->sta_netif, "EFFECTIVE");

    while (o->wifi_ready && !o->mqtt_started) {
        if (resolve_broker_hostname(o)) {
            if (o->wifi_ready) start_mqtt_after_ip(o);
            break;
        }

        if (!o->wifi_ready) break;
        if (promote_dns_fallback_to_main(o)) {
            ESP_LOGW(TAG, "Retrying broker DNS immediately with promoted fallback resolver");
            continue;
        }

        if (resolve_broker_doh(o)) {
            if (o->wifi_ready) start_mqtt_after_ip(o);
            break;
        }

        if (!o->wifi_ready) break;
        ESP_LOGW(TAG,
                 "Broker unresolved by classic DNS and DoH; retrying resolver chain in %u ms while Wi-Fi remains available",
                 (unsigned)OBU_DNS_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(OBU_DNS_RETRY_DELAY_MS));
    }

    o->dns_task_running = false;
    vTaskDelete(NULL);
}

static void start_dns_gate_after_ip(struct obu_otm *o)
{
    if (o == NULL || !o->wifi_ready || o->mqtt_started || o->dns_task_running) return;

    o->dns_task_running = true;
    const BaseType_t created = xTaskCreate(dns_gate_task,
                                           "otm_dns",
                                           OBU_DNS_TASK_STACK_SIZE,
                                           o,
                                           OBU_DNS_TASK_PRIORITY,
                                           NULL);
    if (created != pdPASS) {
        o->dns_task_running = false;
        o->errors++;
        ESP_LOGE(TAG, "Failed to create DNS/DoH resolver task; MQTT remains deferred");
    }
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    struct obu_otm *o = arg;
    if (o == NULL) return;

    if (wifi_debug_enabled()) {
        const char *base_name = base == WIFI_EVENT ? "WIFI_EVENT" : (base == IP_EVENT ? "IP_EVENT" : "OTHER_EVENT");
        ESP_LOGD(TAG, "Wi-Fi debug event: base=%s id=%ld", base_name, (long)id);
    }

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        o->wifi_connect_attempts++;
        ESP_LOGI(TAG, "Wi-Fi STA started; connecting to SSID '%s' (attempt #%u)",
                 o->wifi_ssid, (unsigned)o->wifi_connect_attempts);
        log_wifi_config("STA_START");
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi connect request failed: %s", esp_err_to_name(err));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Wi-Fi associated with SSID '%s'; waiting for DHCP/IP", o->wifi_ssid);
        log_wifi_link();
        log_wifi_config("STA_CONNECTED");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        o->wifi_ready = false;
        o->dns_main_promoted = false;
        o->broker_ip[0] = '\0';
        o->broker_ip_uri[0] = '\0';
        o->wifi_disconnects++;
        stop_mqtt_for_network_loss(o);

        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)data;
        if (disc != NULL && wifi_debug_enabled()) {
            ESP_LOGW(TAG,
                     "Wi-Fi disconnected #%u: reason=%u (%s) RSSI=%d BSSID=%02x:%02x:%02x:%02x:%02x:%02x; reconnecting",
                     (unsigned)o->wifi_disconnects,
                     (unsigned)disc->reason,
                     wifi_disconnect_reason_name(disc->reason),
                     (int)disc->rssi,
                     disc->bssid[0], disc->bssid[1], disc->bssid[2], disc->bssid[3], disc->bssid[4], disc->bssid[5]);
            if (disc->reason == WIFI_REASON_NO_AP_FOUND) {
                ESP_LOGW(TAG,
                         "Wi-Fi debug: configured SSID was not found; verify SSID spelling, hotspot visibility, supported band/channel and that the hotspot is actively advertising");
            } else if (disc->reason == WIFI_REASON_AUTH_FAIL || disc->reason == WIFI_REASON_HANDSHAKE_TIMEOUT) {
                ESP_LOGW(TAG,
                         "Wi-Fi debug: authentication/handshake failed; verify the configured password and hotspot security mode");
            }
        } else if (disc != NULL) {
            ESP_LOGW(TAG, "Wi-Fi disconnected #%u: reason=%u RSSI=%d; reconnecting",
                     (unsigned)o->wifi_disconnects, (unsigned)disc->reason, (int)disc->rssi);
        } else {
            ESP_LOGW(TAG, "Wi-Fi disconnected #%u; reconnecting", (unsigned)o->wifi_disconnects);
        }
        o->wifi_connect_attempts++;
        const esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "Wi-Fi reconnect request failed: %s", esp_err_to_name(err));
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        o->wifi_ready = true;
        o->dns_main_promoted = false;
        o->broker_ip[0] = '\0';
        o->broker_ip_uri[0] = '\0';
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
        log_wifi_config("GOT_IP");
        start_dns_gate_after_ip(o);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP && wifi_debug_enabled()) {
        ESP_LOGW(TAG,
                 "Wi-Fi debug: station lost its IP lease; Wi-Fi association may still be present, watch for a new GOT_IP event or a subsequent disconnect");
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

    enable_wifi_debug_logging();

    snprintf(o->topic, sizeof(o->topic), "its/%s/packet", c->node_id);
    snprintf(o->status, sizeof(o->status), "its/%s/status", c->node_id);
    snprintf(o->broker_uri, sizeof(o->broker_uri), "%s",
             c->broker_uri != NULL ? c->broker_uri : "mqtts://cits1.opentrafficmap.org");
    snprintf(o->wifi_ssid, sizeof(o->wifi_ssid), "%s", c->wifi_ssid);

    if (!broker_hostname_from_uri(o->broker_uri, o->broker_host, sizeof(o->broker_host))) {
        ESP_LOGE(TAG, "Unable to extract broker hostname from URI '%s'", o->broker_uri);
        free(o);
        return ESP_ERR_INVALID_ARG;
    }
    o->broker_port = broker_port_from_uri(o->broker_uri);
    if (o->broker_port == 0U) {
        ESP_LOGE(TAG, "Unable to determine broker port from URI '%s'", o->broker_uri);
        free(o);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,
             "OTM direct uploader config: SSID='%s' broker='%s' broker_host='%s' broker_port=%u node='%s' packet_topic='%s'",
             o->wifi_ssid,
             o->broker_uri,
             o->broker_host,
             (unsigned)o->broker_port,
             c->node_id,
             o->topic);
#ifdef CONFIG_OBU_OTM_TLS_TRACE
    ESP_LOGW(TAG, "OpenTrafficMap TLS trace enabled; mbedTLS handshake diagnostics are active for development");
#endif
#ifdef CONFIG_OBU_DNS_FALLBACK_ENABLE
    ESP_LOGI(TAG, "OTM DNS fallback enabled: %s", CONFIG_OBU_DNS_FALLBACK_IPV4);
#else
    ESP_LOGI(TAG, "OTM DNS fallback disabled; DHCP-provided DNS only");
#endif
    ESP_LOGI(TAG, "OTM secure DoH fallback enabled over HTTPS/443 using Cloudflare wire-format DNS");

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
        ESP_LOGE(TAG, "Wi-Fi initialization failed at NVS init: %s", esp_err_to_name(err));
        free(o);
        return err;
    }

    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed at esp_netif_init: %s", esp_err_to_name(err));
        free(o);
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed creating default event loop: %s", esp_err_to_name(err));
        free(o);
        return err;
    }

    o->sta_netif = esp_netif_create_default_wifi_sta();
    if (o->sta_netif == NULL) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed creating default STA netif");
        free(o);
        return ESP_FAIL;
    }

    wifi_init_config_t wi = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&wi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed at esp_wifi_init: %s", esp_err_to_name(err));
        free(o);
        return err;
    }

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, c->wifi_ssid, sizeof(wc.sta.ssid) - 1);
    if (c->wifi_password != NULL) strncpy((char *)wc.sta.password, c->wifi_password, sizeof(wc.sta.password) - 1);

    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, o);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed registering Wi-Fi event handler: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_evt, o);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed registering IP event handler: %s", esp_err_to_name(err));
        return err;
    }

    esp_mqtt_client_config_t mc = {
        .broker.address.uri = o->broker_uri,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .broker.verification.common_name = o->broker_host,
        .session.last_will.topic = o->status,
        .session.last_will.msg = "offline",
        .session.last_will.retain = 1,
        .session.last_will.qos = 0,
    };
    o->mqtt = esp_mqtt_client_init(&mc);
    if (o->mqtt == NULL) {
        ESP_LOGE(TAG, "OpenTrafficMap MQTT client initialization failed");
        return ESP_FAIL;
    }

    err = esp_mqtt_client_register_event(o->mqtt, ESP_EVENT_ANY_ID, mqtt_evt, o);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OpenTrafficMap MQTT event registration failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed setting STA mode: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed applying STA configuration: %s", esp_err_to_name(err));
        return err;
    }

    log_wifi_config("CONFIGURED");

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed starting driver: %s", esp_err_to_name(err));
        return err;
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
