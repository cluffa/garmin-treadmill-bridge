idf_component_register(
    SRCS "main.c" "ui.c" "display.c" "display_format.c"
         "button.c" "power.c" "battery.c"
    INCLUDE_DIRS "."
    REQUIRES bridge_core bt nvs_flash esp_adc
)
