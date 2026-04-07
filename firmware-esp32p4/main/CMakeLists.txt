idf_component_register(
    SRCS
        "app_main.cpp"
        "ov5647.cpp"
        "camera_task.cpp"
        "inference_task.cpp"
        "radio_task.cpp"
        "power_task.cpp"

    INCLUDE_DIRS "."

    REQUIRES
        freertos
        nvs_flash
        esp_wifi
        esp_netif
        esp_http_client
        esp_pm
        esp_timer
        driver
        esp_driver_cam        # CSI controller (ESP-IDF 5.3+ P4)
        esp_driver_ppa        # Pixel Processing Accelerator (resize/convert)
        # espressif__esp-dl and espressif__esp_cam_sensor are listed in
        # idf_component.yml and auto-linked by the IDF component manager —
        # no REQUIRES entry needed here.
        ulp              # LP core / ULP support
        log
        json
        vfs
)

# ── LP core ULP program ───────────────────────────────────────────────────────
set(ulp_app_name ulp_pir_monitor)
set(ulp_sources  "${CMAKE_CURRENT_SOURCE_DIR}/../ulp/ulp_pir_monitor.c")

ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${CMAKE_CURRENT_SOURCE_DIR}")
