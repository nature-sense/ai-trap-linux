// ─────────────────────────────────────────────────────────────────────────────
// power_task.cpp — PowerTask: idle detection + orderly deep sleep entry
// ─────────────────────────────────────────────────────────────────────────────

#include "power_task.h"
#include "camera_task.h"
#include "pipeline.h"
#include "trap_config.h"

// LP core ULP headers (generated at build time from ulp/ulp_pir_monitor.c)
#include "ulp_lp_core.h"
#include "ulp_pir_monitor.h"  // generated: exports from ulp_pir_monitor.c (shared vars)

#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdio>

// ULP binary embedded by the build system — symbol names derived from ULP app name
extern const uint8_t ulp_pir_monitor_bin_start[] asm("_binary_ulp_pir_monitor_bin_start");
extern const uint8_t ulp_pir_monitor_bin_end[]   asm("_binary_ulp_pir_monitor_bin_end");

static const char* TAG = "power";

PowerTask g_power_task;

// ── LP core ULP program load ──────────────────────────────────────────────────

void PowerTask::load_ulp_pir_program() {
    const size_t bin_size = ulp_pir_monitor_bin_end - ulp_pir_monitor_bin_start;
    ESP_LOGI(TAG, "Loading LP core ULP program (%zu bytes)", bin_size);

    ulp_lp_core_cfg_t cfg = {
        .wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU,
    };
    ESP_ERROR_CHECK(ulp_lp_core_load_binary(ulp_pir_monitor_bin_start, bin_size));

    // Set the PIR GPIO number in ULP shared memory so the LP core knows
    // which pin to watch. The ULP program reads this from RTC memory.
    ulp_pir_gpio_num = g_config.pir_gpio;

    ESP_ERROR_CHECK(ulp_lp_core_run(&cfg));
    ESP_LOGI(TAG, "LP core started — monitoring PIR on GPIO%u", g_config.pir_gpio);
}

// ── Deep sleep entry ──────────────────────────────────────────────────────────

void PowerTask::enter_deep_sleep() {
    ESP_LOGI(TAG, "Entering deep sleep — wakeup on PIR (GPIO%u) via LP core",
             g_config.pir_gpio);

    // Configure LP core (ULP) as wakeup source
    ESP_ERROR_CHECK(esp_sleep_enable_ulp_wakeup());

    // Brief delay so UART flushes before sleep
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();
    // Never returns — HP cores power off; LP core takes over.
}

// ── PowerTask::run() ──────────────────────────────────────────────────────────

void PowerTask::run() {
    wakeup_cause_ = esp_sleep_get_wakeup_cause();

    switch (wakeup_cause_) {
    case ESP_SLEEP_WAKEUP_ULP:
        ESP_LOGI(TAG, "Wakeup: LP core (PIR motion detected)");
        break;
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        ESP_LOGI(TAG, "Wakeup: first boot or power-on reset");
        break;
    default:
        ESP_LOGI(TAG, "Wakeup: cause %d", static_cast<int>(wakeup_cause_));
        break;
    }

    // Record startup time as initial activity so we don't immediately sleep
    last_activity_us_ = esp_timer_get_time();

    const int64_t idle_threshold_us =
        static_cast<int64_t>(g_config.idle_timeout_s) * 1'000'000LL;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000)); // check every 5 s

        int64_t now     = esp_timer_get_time();
        int64_t idle_us = now - last_activity_us_;
        bool queues_idle =
            g_frame_queue.waiting()     == 0 &&
            g_detection_queue.waiting() == 0;

        ESP_LOGD(TAG, "Idle: %.1f s (threshold: %lu s), queues %s",
                 idle_us / 1e6,
                 (unsigned long)g_config.idle_timeout_s,
                 queues_idle ? "empty" : "active");

        if (idle_us < idle_threshold_us) continue;
        if (!queues_idle) {
            // Activity in queues — reset idle timer to avoid premature sleep
            last_activity_us_ = now;
            continue;
        }

        // ── Idle timeout reached — orderly shutdown ───────────────────────
        ESP_LOGI(TAG, "Idle for %.1f s — initiating shutdown", idle_us / 1e6);

        // 1. Signal CameraTask to stop new captures
        g_shutdown_requested = true;
        g_camera_task.request_stop();

        // 2. Wait for frame_queue to drain (InferenceTask finishes current frame)
        ESP_LOGI(TAG, "Waiting for frame queue to drain…");
        for (int i = 0; i < 20; ++i) {
            if (g_frame_queue.waiting() == 0) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // 3. Wait for detection_queue to drain (RadioTask sends/buffers all events)
        ESP_LOGI(TAG, "Waiting for detection queue to drain…");
        for (int i = 0; i < 60; ++i) {
            if (g_detection_queue.waiting() == 0) break;
            vTaskDelay(pdMS_TO_TICKS(500));
        }

        // 4. Load LP core program and enter deep sleep
        load_ulp_pir_program();
        enter_deep_sleep();

        // Never reached
        break;
    }
}
