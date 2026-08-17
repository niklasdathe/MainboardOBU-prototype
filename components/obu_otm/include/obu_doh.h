#pragma once

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Resolve an IPv4 address through DNS-over-HTTPS without relying on the
 * network's classic UDP/TCP port-53 resolver path.
 *
 * The implementation connects to a known resolver IP over HTTPS while still
 * validating the resolver's TLS certificate against its DNS name.
 */
esp_err_t obu_doh_resolve_ipv4(const char *hostname, char *out_ipv4, size_t out_ipv4_size);

#ifdef __cplusplus
}
#endif
