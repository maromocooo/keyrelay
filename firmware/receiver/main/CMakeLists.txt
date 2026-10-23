idf_component_register(SRCS "main.c" "ccid_dev.c" "hid_dev.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES yd_link esp_driver_gpio esp_timer)
