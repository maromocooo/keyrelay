idf_component_register(SRCS "yd_link.c" "yd_link_ble.c" "yd_sec.c" "yd_frag.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_driver_uart bt nvs_flash esp_timer esp_partition mbedtls esp_hw_support)
