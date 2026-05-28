idf_component_register(
    SRCS "salmon_run.c" "lv_font_pingfang_16_cjk.c" "lv_font_pingfang_18_cjk.c" "lv_font_pingfang_22_cjk.c" "lv_font_pingfang_24_cjk.c"
    INCLUDE_DIRS "." "../managed_components/lvgl__lvgl/src/extra/libs/png"
    REQUIRES lvgl esp_http_client esp_wifi esp_event esp_netif nvs_flash esp_timer esp_lcd esp_driver_spi esp_driver_i2c driver esp_pm lwip cJSON mbedtls)
