idf_component_register(SRCS "obu_otm.c" "obu_doh.c" INCLUDE_DIRS "include" REQUIRES obu_core mqtt esp_wifi esp_event esp_netif esp_http_client nvs_flash mbedtls)
