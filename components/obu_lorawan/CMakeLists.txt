idf_component_register(
    SRCS "obu_lorawan.cpp"
    INCLUDE_DIRS "include"
    REQUIRES RadioLib esp_driver_spi esp_driver_gpio
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)
