idf_component_register(
    SRCS "main.c" "ipc.c" "pass_usb_device.c" "km_inject.c"
    INCLUDE_DIRS "." "../include"
    REQUIRES driver esp_tinyusb
)
