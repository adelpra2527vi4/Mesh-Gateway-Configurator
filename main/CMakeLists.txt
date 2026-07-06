set(srcs "main.c" "relay_handler.c" "ble_classic_handler.c" "modem_mqtt.c" "usb_cdc.c")

idf_component_register(
    SRCS "${srcs}"
    INCLUDE_DIRS "." "include"
    REQUIRES
        bt
        nvs_flash
        esp_event
        esp_timer
        esp_driver_gpio
        esp_driver_uart
        esp_driver_rmt
        driver
        esp_tinyusb
)