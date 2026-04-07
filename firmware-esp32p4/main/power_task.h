#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// power_task.h — PowerTask: idle detection and deep sleep orchestration
//
// Responsibilities:
//   • Monitor detection activity (timestamp of last DetectionEvent sent)
//   • When no detection for idle_timeout_s seconds AND queues are empty:
//       1. Signal CameraTask to stop
//       2. Wait for InferenceTask to drain frame_queue
//       3. Wait for RadioTask to drain detection_queue
//       4. Load LP core ULP program (PIR monitor)
//       5. Configure wakeup source and call esp_deep_sleep_start()
//   • On wakeup: report reason and reset idle timer
//
// Only PowerTask calls esp_deep_sleep_start() — never any other task.
//
// Core pinning: Core 0, priority 1 (lowest — never competes with real work)
// Stack: 4 KB
// ─────────────────────────────────────────────────────────────────────────────

#include "pipeline.h"
#include "trap_config.h"
#include "esp_sleep.h"
#include "esp_timer.h"

class PowerTask : public Task<4096> {
public:
    PowerTask() = default;
    void begin() { start("power", 1, 0); }

    /// Called by RadioTask each time a DetectionEvent is successfully processed.
    void record_activity() { last_activity_us_ = esp_timer_get_time(); }

    /// Reason for the most recent HP core wakeup.
    esp_sleep_wakeup_cause_t wakeup_cause() const { return wakeup_cause_; }

protected:
    void run() override;

private:
    volatile int64_t last_activity_us_ = 0;
    esp_sleep_wakeup_cause_t wakeup_cause_ = ESP_SLEEP_WAKEUP_UNDEFINED;

    void enter_deep_sleep();
    void load_ulp_pir_program();
};

extern PowerTask g_power_task;
