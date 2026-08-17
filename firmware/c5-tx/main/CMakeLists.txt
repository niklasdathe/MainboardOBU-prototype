idf_component_register(
    SRCS "main.c" "vam_encoder.c"
    INCLUDE_DIRS "."
    REQUIRES obu_radio obu_core esp_driver_gpio esp_timer
)
