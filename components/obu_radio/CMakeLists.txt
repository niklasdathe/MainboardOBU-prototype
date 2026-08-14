if(IDF_TARGET STREQUAL "esp32c5")
    idf_component_register(
        SRCS "obu_radio_esp32c5.c"
        INCLUDE_DIRS "include"
        REQUIRES obu_core esp_wifi esp_event nvs_flash esp_timer freertos esp_app_format
    )
else()
    # Keep the reusable public interface visible to other targets without compiling
    # the ESP32-C5-only private 802.11p backend.
    idf_component_register(INCLUDE_DIRS "include" REQUIRES obu_core)
endif()
