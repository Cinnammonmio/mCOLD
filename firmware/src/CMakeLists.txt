idf_component_register(SRCS "main.cpp" "health.cpp" "rails.cpp" "bus.cpp"
                            "temp.cpp" "rtcclock.cpp" "power.cpp"
                            "accel.cpp" "gnss.cpp" "nfc.cpp" "leds.cpp" "chargeled.cpp"
                            "buzzer.cpp" "timekeep.cpp" "config.cpp"
                            "flashlog.cpp" "flashlog_test.cpp"
                            "door.cpp" "trip.cpp" "indicate.cpp"
                            "canvas.cpp" "epd.cpp" "screens.cpp" "display.cpp"
                            "rpc.cpp" "ble.cpp" "auth.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_driver_i2c esp_driver_spi
                                esp_driver_gpio esp_driver_usb_serial_jtag
                                esp_driver_uart esp_driver_rmt esp_driver_ledc
                                esp_app_format esp_timer nvs_flash json mbedtls bt
                                esp_partition esp_system)
