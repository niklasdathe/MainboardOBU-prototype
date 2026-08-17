idf_component_register(
    SRCS
        "obu_lorawan.cpp"
        "obu_lorawan_persistence.cpp"
    INCLUDE_DIRS "include"
    REQUIRES RadioLib esp_driver_spi esp_driver_gpio esp_driver_ledc esp_timer nvs_flash
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
