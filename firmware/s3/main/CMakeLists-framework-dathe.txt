idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES obu_core obu_ipc obu_hmi obu_warning obu_v2x obu_time obu_gnss obu_log obu_ifaces obu_lorawan
             esp_driver_i2c esp_driver_spi esp_driver_gpio esp_app_format
)
