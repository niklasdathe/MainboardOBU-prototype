#include "obu_doh.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "obu_doh";

#define OBU_DOH_TLS_NAME "cloudflare-dns.com"
#define OBU_DOH_PATH "/dns-query"
#define OBU_DOH_TIMEOUT_MS 8000
#define OBU_DOH_QUERY_MAX 256U
#define OBU_DOH_RESPONSE_MAX 768U
#define OBU_DNS_TXID 0xB10BU

static const char *const s_doh_resolver_ips[] = {
    "1.1.1.1",
    "1.0.0.1",
};

typedef struct {
    uint8_t data[OBU_DOH_RESPONSE_MAX];
    size_t len;
    bool overflow;
} doh_response_t;

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static bool dns_skip_name(const uint8_t *msg, size_t msg_len, size_t *offset)
{
    if (msg == NULL || offset == NULL) return false;

    size_t pos = *offset;
    while (pos < msg_len) {
        const uint8_t label = msg[pos];
        if ((label & 0xC0U) == 0xC0U) {
            if (pos + 1U >= msg_len) return false;
            *offset = pos + 2U;
            return true;
        }
        if (label == 0U) {
            *offset = pos + 1U;
            return true;
        }
        if (label > 63U || pos + 1U + label > msg_len) return false;
        pos += 1U + label;
    }
    return false;
}

static size_t build_dns_a_query(const char *hostname, uint8_t *out, size_t out_size)
{
    if (hostname == NULL || hostname[0] == '\0' || out == NULL || out_size < 17U) return 0U;

    memset(out, 0, out_size);
    out[0] = (uint8_t)(OBU_DNS_TXID >> 8);
    out[1] = (uint8_t)(OBU_DNS_TXID & 0xFFU);
    out[2] = 0x01U; /* RD */
    out[5] = 0x01U; /* QDCOUNT = 1 */

    size_t pos = 12U;
    const char *label_start = hostname;
    const char *p = hostname;

    while (true) {
        if (*p == '.' || *p == '\0') {
            const size_t label_len = (size_t)(p - label_start);
            if (label_len == 0U || label_len > 63U || pos + 1U + label_len + 5U > out_size) return 0U;
            out[pos++] = (uint8_t)label_len;
            memcpy(&out[pos], label_start, label_len);
            pos += label_len;

            if (*p == '\0') break;
            label_start = p + 1;
        }
        ++p;
    }

    out[pos++] = 0U;
    out[pos++] = 0U;
    out[pos++] = 1U; /* QTYPE A */
    out[pos++] = 0U;
    out[pos++] = 1U; /* QCLASS IN */
    return pos;
}

static bool parse_dns_a_response(const uint8_t *msg, size_t msg_len, char *out_ipv4, size_t out_ipv4_size)
{
    if (msg == NULL || out_ipv4 == NULL || out_ipv4_size < 16U || msg_len < 12U) return false;
    if (read_be16(msg) != OBU_DNS_TXID) return false;

    const uint16_t flags = read_be16(&msg[2]);
    if ((flags & 0x8000U) == 0U || (flags & 0x000FU) != 0U) return false;

    const uint16_t qdcount = read_be16(&msg[4]);
    const uint16_t ancount = read_be16(&msg[6]);
    size_t offset = 12U;

    for (uint16_t i = 0; i < qdcount; ++i) {
        if (!dns_skip_name(msg, msg_len, &offset) || offset + 4U > msg_len) return false;
        offset += 4U;
    }

    for (uint16_t i = 0; i < ancount; ++i) {
        if (!dns_skip_name(msg, msg_len, &offset) || offset + 10U > msg_len) return false;

        const uint16_t type = read_be16(&msg[offset]);
        const uint16_t klass = read_be16(&msg[offset + 2U]);
        const uint16_t rdlength = read_be16(&msg[offset + 8U]);
        offset += 10U;
        if (offset + rdlength > msg_len) return false;

        if (type == 1U && klass == 1U && rdlength == 4U) {
            const int written = snprintf(out_ipv4,
                                         out_ipv4_size,
                                         "%u.%u.%u.%u",
                                         (unsigned)msg[offset],
                                         (unsigned)msg[offset + 1U],
                                         (unsigned)msg[offset + 2U],
                                         (unsigned)msg[offset + 3U]);
            return written > 0 && (size_t)written < out_ipv4_size;
        }
        offset += rdlength;
    }

    return false;
}

static esp_err_t doh_http_event(esp_http_client_event_t *evt)
{
    if (evt == NULL || evt->user_data == NULL) return ESP_OK;
    doh_response_t *response = (doh_response_t *)evt->user_data;

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data != NULL && evt->data_len > 0) {
        const size_t incoming = (size_t)evt->data_len;
        if (response->len + incoming > sizeof(response->data)) {
            response->overflow = true;
            return ESP_OK;
        }
        memcpy(&response->data[response->len], evt->data, incoming);
        response->len += incoming;
    }
    return ESP_OK;
}

static esp_err_t doh_query_one(const char *resolver_ip,
                               const char *hostname,
                               const uint8_t *query,
                               size_t query_len,
                               char *out_ipv4,
                               size_t out_ipv4_size)
{
    doh_response_t response = {0};
    esp_http_client_config_t config = {
        .host = resolver_ip,
        .port = 443,
        .path = OBU_DOH_PATH,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = OBU_DOH_TIMEOUT_MS,
        .event_handler = doh_http_event,
        .user_data = &response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .common_name = OBU_DOH_TLS_NAME,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_set_header(client, "Host", OBU_DOH_TLS_NAME);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept", "application/dns-message");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/dns-message");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Connection", "close");
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, (const char *)query, (int)query_len);
    if (err == ESP_OK) err = esp_http_client_perform(client);

    const int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "DoH resolver %s returned HTTP status %d for '%s'", resolver_ip, status, hostname);
        err = ESP_FAIL;
    }
    if (err == ESP_OK && response.overflow) {
        ESP_LOGW(TAG, "DoH resolver %s response exceeded %u bytes", resolver_ip, (unsigned)sizeof(response.data));
        err = ESP_ERR_NO_MEM;
    }
    if (err == ESP_OK && !parse_dns_a_response(response.data, response.len, out_ipv4, out_ipv4_size)) {
        ESP_LOGW(TAG, "DoH resolver %s returned no usable IPv4 A record for '%s' (%u bytes)",
                 resolver_ip, hostname, (unsigned)response.len);
        err = ESP_ERR_NOT_FOUND;
    }

    esp_http_client_cleanup(client);
    return err;
}

esp_err_t obu_doh_resolve_ipv4(const char *hostname, char *out_ipv4, size_t out_ipv4_size)
{
    if (hostname == NULL || hostname[0] == '\0' || out_ipv4 == NULL || out_ipv4_size < 16U) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t query[OBU_DOH_QUERY_MAX] = {0};
    const size_t query_len = build_dns_a_query(hostname, query, sizeof(query));
    if (query_len == 0U) {
        ESP_LOGE(TAG, "Unable to encode DoH DNS query for '%s'", hostname);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t last_err = ESP_FAIL;
    for (size_t i = 0; i < sizeof(s_doh_resolver_ips) / sizeof(s_doh_resolver_ips[0]); ++i) {
        ESP_LOGW(TAG,
                 "Classic DNS unavailable; trying secure DoH resolver %s:443 for '%s' with TLS name '%s'",
                 s_doh_resolver_ips[i], hostname, OBU_DOH_TLS_NAME);
        last_err = doh_query_one(s_doh_resolver_ips[i], hostname, query, query_len, out_ipv4, out_ipv4_size);
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "DoH resolved '%s' -> %s via %s:443", hostname, out_ipv4, s_doh_resolver_ips[i]);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "DoH resolver %s failed for '%s': %s",
                 s_doh_resolver_ips[i], hostname, esp_err_to_name(last_err));
    }

    return last_err;
}
