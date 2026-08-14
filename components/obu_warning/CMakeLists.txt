idf_component_register(
    SRCS "obu_warning.c"
    INCLUDE_DIRS "include"
    REQUIRES obu_core esp_driver_ledc freertos
)
