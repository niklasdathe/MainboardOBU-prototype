idf_component_register(SRCS "obu_radio_esp32c5.c" INCLUDE_DIRS "include" REQUIRES obu_core esp_wifi esp_event nvs_flash esp_timer freertos esp_app_format)
