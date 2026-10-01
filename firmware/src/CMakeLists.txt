idf_component_register(SRCS "main.cpp" "health.cpp" "rails.cpp" "bus.cpp"
                            "temp.cpp" "rtcclock.cpp" "power.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c esp_driver_spi
                                esp_driver_gpio esp_driver_usb_serial_jtag
                                esp_timer nvs_flash esp_partition esp_system)
