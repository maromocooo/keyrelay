idf_component_register(SRCS "main.c" "ccid_host.c" "otp_host.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES usb yd_link esp_hw_support esp_driver_gpio esp_driver_uart esp_timer esp_pm)
