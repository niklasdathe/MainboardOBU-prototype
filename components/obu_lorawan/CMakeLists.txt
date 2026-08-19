idf_component_register(
    SRCS
        "obu_lorawan.cpp"
        "obu_lorawan_persistence.cpp"
    INCLUDE_DIRS "include"
    REQUIRES RadioLib esp_driver_spi esp_driver_gpio esp_driver_ledc esp_timer nvs_flash
)

target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_17)

# RadioLib debug switches are compile-time options in BuildOpt.h. Apply the
# selected definitions both to RadioLib itself and to this component so every
# translation unit sees a consistent debug configuration.
set(radiolib_debug_definitions)
if(CONFIG_OBU_LORAWAN_RADIOLIB_PROTOCOL_DEBUG)
    list(APPEND radiolib_debug_definitions
        RADIOLIB_DEBUG_BASIC=1
        RADIOLIB_DEBUG_PROTOCOL=1
    )
endif()
if(CONFIG_OBU_LORAWAN_RADIOLIB_SPI_DEBUG)
    list(APPEND radiolib_debug_definitions
        RADIOLIB_DEBUG_BASIC=1
        RADIOLIB_DEBUG_SPI=1
    )
endif()

if(radiolib_debug_definitions)
    idf_component_get_property(radiolib_lib RadioLib COMPONENT_LIB)
    target_compile_definitions(${radiolib_lib} PRIVATE ${radiolib_debug_definitions})
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ${radiolib_debug_definitions})
endif()
