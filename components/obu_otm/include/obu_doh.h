#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resolve an IPv4 address using the OTM hotspot-recovery resolver chain.
 *
 * The normal lwIP/DHCP resolver is attempted by the caller first. This helper
 * then tries IPv6 RDNSS when available, DNS-over-TCP using the configured
 * resolver slots, and finally DNS-over-HTTPS against multiple public resolver
 * providers using bootstrap IPs. HTTPS certificate verification and SNI remain
 * enabled through each provider's DNS name.
 */
esp_err_t obu_doh_resolve_ipv4(const char *hostname, char *out_ipv4, size_t out_ipv4_size);

#ifdef __cplusplus
}
#endif
