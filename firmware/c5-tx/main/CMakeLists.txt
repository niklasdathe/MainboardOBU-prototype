idf_component_register(
    SRCS "main.c" "vam_encoder.c"
    INCLUDE_DIRS "."
    REQUIRES obu_radio obu_core esp_driver_gpio esp_timer
)

# The manual UPER field is hidden by Kconfig when the built-in generator is
# selected, so sdkconfig.h intentionally has no string macro for it. main.c
# keeps one profile table for both modes; provide an unused empty definition
# only in generated mode.
if(CONFIG_C5_TX_VAM_GENERATED)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CONFIG_C5_TX_VAM_PDU_HEX="")
endif()
