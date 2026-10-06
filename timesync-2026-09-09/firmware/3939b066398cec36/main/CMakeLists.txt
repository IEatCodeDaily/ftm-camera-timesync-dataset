# Component CMakeLists for the ftm_clocksync main component.
idf_component_register(
    SRCS
        "app_main.c"
        "recovery.c"
        "console_cmd.c"
        "ftm_sync.c"
        "clock_model.c"
        "node_config.c"
        "telemetry.c"
        "strobe_gpio.c"
        "simple_ntp.c"
        "mode_ctrl.c"
        "wifi_setup.c"
        "wifi_config.c"
        "lan_coord.c"
        "lan_cmd.c"
        "camera_capture.c"
        "camera_server.c"
        "camera_udp.c"
        "camera_frex_sync.c"
        "camera_cmd.c"
        "intrinsic_store.c"
        "cmd_server.c"
        "centroid_detector.c"
        "centroid_pie.S"
        "centroid_packet.c"
        "live_track.c"
        "sync_capture.c"
        "usb_detect.c"
        "uvc_webcam.c"
        "usb_cdc_console.c"
        "ota_server.c"
        "discovery_beacon.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        console
        driver
        esp_driver_uart
        esp_event
        esp_timer
        esp_wifi
        lwip
        nvs_flash
        esp32-camera
        espressif__esp_jpeg
        usb_device_uvc
        espressif__tinyusb
        app_update
        esp_http_server
)

# The tracking path is deliberately throughput-first.  ESP-IDF's global
# performance profile is -O2; use -O3 for this component so JPEG callback,
# row scanning and packet assembly can inline across their small helpers.
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -funroll-loops)

# JPEG luminance decode is the dominant tracking cost (~42 ms at QVGA in the
# on-device profiler). ESP-IDF's performance profile compiles managed
# components at -O2, so explicitly give the decoder the same throughput-first
# treatment as the detector.
idf_component_get_property(esp_jpeg_lib espressif__esp_jpeg COMPONENT_LIB)
target_compile_options(${esp_jpeg_lib} PRIVATE -O3 -funroll-loops)
