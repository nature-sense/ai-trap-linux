// ─────────────────────────────────────────────────────────────────────────────
// app_main.cpp — ai-trap ESP32-P4 entry point
//
// Startup sequence (matches esp32-p4-runtime-architecture.md § Startup):
//   1. NVS init
//   2. Power management configure (max/min freq + auto light-sleep)
//   3. Determine wakeup reason
//   4. Create static queues and global config
//   5. Load config from NVS
//   6. Create and start all tasks (PowerTask last)
//   7. Return — FreeRTOS scheduler takes over
//
// All task and queue storage is static (no heap).
// app_main() must return quickly; it runs at the IDF main task priority.
// ─────────────────────────────────────────────────────────────────────────────

#include "pipeline.h"
#include "trap_config.h"
#include "camera_task.h"
#include "inference_task.h"
#include "radio_task.h"
#include "power_task.h"

#include "nvs_flash.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "main";

// ── Global queue definitions ──────────────────────────────────────────────────
// Declared extern in pipeline.h; defined once here.

StaticQueue<FrameBuffer*, 2>    g_frame_queue;
StaticQueue<FrameBuffer*, 2>    g_buffer_pool_queue;
StaticQueue<DetectionEvent, 16> g_detection_queue;

volatile uint32_t g_frame_drop_count = 0;
volatile uint32_t g_event_drop_count = 0;
volatile bool     g_shutdown_requested = false;

// ── Global config ─────────────────────────────────────────────────────────────
TrapConfig g_config;

// ── app_main ─────────────────────────────────────────────────────────────────

extern "C" void app_main() {
    // ── 1. NVS init ───────────────────────────────────────────────────────
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition erased and reinitialised");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    // ── 2. Power management ───────────────────────────────────────────────
    // Auto light-sleep: CPU drops to min_freq when all tasks are blocked.
    // ESP_PM_CPU_FREQ_MAX lock is acquired by InferenceTask to prevent throttle
    // during inference. Between frames the system light-sleeps automatically.
    esp_pm_config_t pm_cfg = {
        .max_freq_mhz       = 400,  // HP core max (P4 @ 400 MHz)
        .min_freq_mhz       = 40,   // LP floor during idle (saves ~80% power)
        .light_sleep_enable = true,
    };
    ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));
    ESP_LOGI(TAG, "PM configured: max=400 MHz, min=40 MHz, light-sleep=on");

    // ── 3. Wakeup reason (logged; PowerTask will act on it) ───────────────
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Boot reason: %d (%s)", static_cast<int>(cause),
             cause == ESP_SLEEP_WAKEUP_ULP ? "ULP/PIR" :
             cause == ESP_SLEEP_WAKEUP_UNDEFINED ? "power-on" : "other");

    // ── 4. Load config from NVS ───────────────────────────────────────────
    trap_config_load(g_config);
    ESP_LOGI(TAG, "Config: trap_id=%u, conf_threshold=%.2f, idle=%lu s",
             g_config.trap_id,
             static_cast<double>(g_config.conf_threshold),
             (unsigned long)g_config.idle_timeout_s);

    if (g_config.wifi_ssid[0] == '\0') {
        ESP_LOGW(TAG, "WiFi SSID not configured — detection events will buffer to NVS");
        ESP_LOGW(TAG, "Set wifi_ssid + wifi_pass via NVS (see README flash instructions)");
    }

    // ── 5. Log memory budget ──────────────────────────────────────────────
    ESP_LOGI(TAG, "Free SRAM: %zu KB | Free PSRAM: %zu KB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM)   / 1024);

    // ── 6. Start tasks ────────────────────────────────────────────────────
    // Order matters: queues must exist before tasks start; PowerTask last.
    ESP_LOGI(TAG, "Starting tasks…");

    g_camera_task.begin();      // Core 0, pri 6
    g_inference_task.begin();   // Core 1, pri 5
    g_radio_task.begin();       // Core 0, pri 3
    g_power_task.begin();       // Core 0, pri 1 — must be last

    ESP_LOGI(TAG, "All tasks started — FreeRTOS scheduler running");
    ESP_LOGI(TAG, "ai-trap v0.1.0 | trap_id=%u | ESP32-P4 + OV5647",
             g_config.trap_id);

    // app_main returns here; FreeRTOS continues running the tasks.
    // The IDF app_main task is deleted automatically on return.
}
