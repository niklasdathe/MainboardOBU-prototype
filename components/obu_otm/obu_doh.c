#include "obu_doh.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "obu_doh";

#define OBU_DOH_PATH "/dns-query"
#define OBU_DOH_TIMEOUT_MS 5000
#define OBU_DNS_TCP_TIMEOUT_MS 3000
#define OBU_IPV6_RDNSS_SETTLE_MS 1500
#define OBU_DOH_QUERY_MAX 256U
#define OBU_DOH_RESPONSE_MAX 768U
#define OBU_DNS_TXID 0xB10BU

typedef struct {
    const char *tls_name;
    const char *ips[2];
} doh_provider_t;

static const doh_provider_t s_doh_providers[] = {
    {
        .tls_name = "cloudflare-dns.com",
        .ips = {"1.1.1.1", "1.0.0.1"},
    },
    {
        .tls_name = "dns.google",
        .ips = {"8.8.8.8", "8.8.4.4"},
    },
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

static bool dns_server_ipv4(esp_netif_dns_type_t type, char *out, size_t out_size)
{
    if (out == NULL || out_size < 16U) return false;

    esp_netif_dns_info_t dns = {0};
    if (esp_netif_get_dns_info(NULL, type, &dns) != ESP_OK ||
        dns.ip.type != IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0U) {
        return false;
    }

    return esp_ip4addr_ntoa(&dns.ip.u_addr.ip4, out, out_size) != NULL;
}

static int tcp_connect_ipv4(const char *server_ip, uint16_t port, int timeout_ms)
{
    if (server_ip == NULL || server_ip[0] == '\0') return -1;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    const int original_flags = fcntl(sock, F_GETFL, 0);
    if (original_flags < 0 || fcntl(sock, F_SETFL, original_flags | O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    int rc = connect(sock, (const struct sockaddr *)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }

    if (rc < 0) {
        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);
        struct timeval timeout = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        rc = select(sock + 1, NULL, &write_fds, NULL, &timeout);
        if (rc <= 0) {
            close(sock);
            return -1;
        }

        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0 || socket_error != 0) {
            close(sock);
            return -1;
        }
    }

    (void)fcntl(sock, F_SETFL, original_flags);

    struct timeval io_timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    (void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
    return sock;
}

static bool send_all(int sock, const uint8_t *data, size_t len)
{
    size_t sent = 0U;
    while (sent < len) {
        const int n = send(sock, data + sent, len - sent, 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(int sock, uint8_t *data, size_t len)
{
    size_t received = 0U;
    while (received < len) {
        const int n = recv(sock, data + received, len - received, 0);
        if (n <= 0) return false;
        received += (size_t)n;
    }
    return true;
}

static esp_err_t dns_tcp_query_one(const char *resolver_ip,
                                   const char *hostname,
                                   const uint8_t *query,
                                   size_t query_len,
                                   char *out_ipv4,
                                   size_t out_ipv4_size)
{
    int sock = tcp_connect_ipv4(resolver_ip, 53U, OBU_DNS_TCP_TIMEOUT_MS);
    if (sock < 0) return ESP_ERR_TIMEOUT;

    uint8_t length_prefix[2] = {
        (uint8_t)(query_len >> 8),
        (uint8_t)(query_len & 0xFFU),
    };
    esp_err_t result = ESP_FAIL;

    if (!send_all(sock, length_prefix, sizeof(length_prefix)) || !send_all(sock, query, query_len)) {
        goto done;
    }

    uint8_t response_length_bytes[2] = {0};
    if (!recv_all(sock, response_length_bytes, sizeof(response_length_bytes))) goto done;
    const size_t response_length = (size_t)read_be16(response_length_bytes);
    if (response_length < 12U || response_length > OBU_DOH_RESPONSE_MAX) {
        result = ESP_ERR_INVALID_SIZE;
        goto done;
    }

    uint8_t response[OBU_DOH_RESPONSE_MAX] = {0};
    if (!recv_all(sock, response, response_length)) goto done;
    if (!parse_dns_a_response(response, response_length, out_ipv4, out_ipv4_size)) {
        result = ESP_ERR_NOT_FOUND;
        goto done;
    }

    result = ESP_OK;

done:
    close(sock);
    return result;
}

static esp_err_t try_dns_over_tcp(const char *hostname,
                                  const uint8_t *query,
                                  size_t query_len,
                                  char *out_ipv4,
                                  size_t out_ipv4_size)
{
    char resolver_ips[3][16] = {{0}};
    size_t resolver_count = 0U;

    const esp_netif_dns_type_t types[] = {
        ESP_NETIF_DNS_BACKUP,
        ESP_NETIF_DNS_MAIN,
        ESP_NETIF_DNS_FALLBACK,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        char ip[16] = {0};
        if (!dns_server_ipv4(types[i], ip, sizeof(ip))) continue;

        bool duplicate = false;
        for (size_t j = 0; j < resolver_count; ++j) {
            if (strcmp(resolver_ips[j], ip) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && resolver_count < sizeof(resolver_ips) / sizeof(resolver_ips[0])) {
            snprintf(resolver_ips[resolver_count], sizeof(resolver_ips[resolver_count]), "%s", ip);
            resolver_count++;
        }
    }

    esp_err_t last_err = ESP_ERR_NOT_FOUND;
    for (size_t i = 0; i < resolver_count; ++i) {
        ESP_LOGW(TAG,
                 "UDP DNS unavailable; trying DNS-over-TCP resolver %s:53 for '%s'",
                 resolver_ips[i], hostname);
        last_err = dns_tcp_query_one(resolver_ips[i], hostname, query, query_len, out_ipv4, out_ipv4_size);
        if (last_err == ESP_OK) {
            ESP_LOGI(TAG, "DNS-over-TCP resolved '%s' -> %s via %s:53",
                     hostname, out_ipv4, resolver_ips[i]);
            return ESP_OK;
        }
        ESP_LOGW(TAG, "DNS-over-TCP resolver %s failed for '%s': %s",
                 resolver_ips[i], hostname, esp_err_to_name(last_err));
    }
    return last_err;
}

static bool ipv6_dns_available(void)
{
#ifdef CONFIG_LWIP_IPV6
    const esp_netif_dns_type_t types[] = {
        ESP_NETIF_DNS_MAIN,
        ESP_NETIF_DNS_BACKUP,
        ESP_NETIF_DNS_FALLBACK,
    };
    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        esp_netif_dns_info_t dns = {0};
        if (esp_netif_get_dns_info(NULL, types[i], &dns) == ESP_OK && dns.ip.type == IPADDR_TYPE_V6) {
            return true;
        }
    }
#endif
    return false;
}

static esp_err_t try_ipv6_rdnss(const char *hostname, char *out_ipv4, size_t out_ipv4_size)
{
#if defined(CONFIG_LWIP_IPV6) && defined(CONFIG_LWIP_IPV6_AUTOCONFIG)
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == NULL) return ESP_ERR_NOT_FOUND;

    const esp_err_t linklocal_err = esp_netif_create_ip6_linklocal(sta);
    if (linklocal_err != ESP_OK && linklocal_err != ESP_ERR_ESP_NETIF_IF_NOT_READY) {
        ESP_LOGW(TAG, "Unable to start IPv6 link-local/SLAAC recovery: %s", esp_err_to_name(linklocal_err));
    }

    ESP_LOGI(TAG,
             "Waiting %u ms for IPv6 SLAAC/RDNSS before public HTTPS resolver fallback",
             (unsigned)OBU_IPV6_RDNSS_SETTLE_MS);
    vTaskDelay(pdMS_TO_TICKS(OBU_IPV6_RDNSS_SETTLE_MS));

    if (!ipv6_dns_available()) return ESP_ERR_NOT_FOUND;

    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    const int rc = getaddrinfo(hostname, NULL, &hints, &result);
    if (rc != 0 || result == NULL) {
        if (result != NULL) freeaddrinfo(result);
        return ESP_ERR_NOT_FOUND;
    }

    const struct sockaddr_in *addr = (const struct sockaddr_in *)result->ai_addr;
    const char *ntop = inet_ntop(AF_INET, &addr->sin_addr, out_ipv4, out_ipv4_size);
    freeaddrinfo(result);
    if (ntop == NULL) return ESP_FAIL;

    ESP_LOGI(TAG, "IPv6 RDNSS resolved '%s' -> %s", hostname, out_ipv4);
    return ESP_OK;
#else
    (void)hostname;
    (void)out_ipv4;
    (void)out_ipv4_size;
    return ESP_ERR_NOT_SUPPORTED;
#endif
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

static esp_err_t doh_query_one(const doh_provider_t *provider,
                               const char *resolver_ip,
                               const char *hostname,
                               const uint8_t *query,
                               size_t query_len,
                               char *out_ipv4,
                               size_t out_ipv4_size)
{
    if (provider == NULL) return ESP_ERR_INVALID_ARG;

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
        .common_name = provider->tls_name,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return ESP_ERR_NO_MEM;

    esp_err_t err = esp_http_client_set_header(client, "Host", provider->tls_name);
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Accept", "application/dns-message");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Content-Type", "application/dns-message");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Connection", "close");
    if (err == ESP_OK) err = esp_http_client_set_post_field(client, (const char *)query, (int)query_len);
    if (err == ESP_OK) err = esp_http_client_perform(client);

    const int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status != 200) {
        ESP_LOGW(TAG, "DoH resolver %s (%s) returned HTTP status %d for '%s'",
                 resolver_ip, provider->tls_name, status, hostname);
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
        ESP_LOGE(TAG, "Unable to encode DNS recovery query for '%s'", hostname);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t last_err = try_ipv6_rdnss(hostname, out_ipv4, out_ipv4_size);
    if (last_err == ESP_OK) return ESP_OK;

    last_err = try_dns_over_tcp(hostname, query, query_len, out_ipv4, out_ipv4_size);
    if (last_err == ESP_OK) return ESP_OK;

    for (size_t provider_index = 0;
         provider_index < sizeof(s_doh_providers) / sizeof(s_doh_providers[0]);
         ++provider_index) {
        const doh_provider_t *provider = &s_doh_providers[provider_index];
        for (size_t ip_index = 0; ip_index < sizeof(provider->ips) / sizeof(provider->ips[0]); ++ip_index) {
            const char *resolver_ip = provider->ips[ip_index];
            ESP_LOGW(TAG,
                     "Classic DNS/TCP unavailable; trying secure DoH resolver %s:443 for '%s' with TLS name '%s'",
                     resolver_ip, hostname, provider->tls_name);
            last_err = doh_query_one(provider, resolver_ip, hostname, query, query_len, out_ipv4, out_ipv4_size);
            if (last_err == ESP_OK) {
                ESP_LOGI(TAG, "DoH resolved '%s' -> %s via %s:443 (%s)",
                         hostname, out_ipv4, resolver_ip, provider->tls_name);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "DoH resolver %s (%s) failed for '%s': %s",
                     resolver_ip, provider->tls_name, hostname, esp_err_to_name(last_err));
        }
    }

    ESP_LOGE(TAG,
             "All DNS recovery transports failed for '%s': DHCP/public UDP, IPv6 RDNSS, DNS-over-TCP, Cloudflare DoH and Google DoH",
             hostname);
    return last_err;
}
