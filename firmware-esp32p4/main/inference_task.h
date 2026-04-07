#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// inference_task.h — InferenceTask: PPA resize + ESPDet-Pico + NMS
//
// Responsibilities:
//   • Pop frames from g_frame_queue
//   • Use PPA to convert YUV420 → RGB888 (same 224×224 — format change only)
//   • Return the capture buffer to g_buffer_pool_queue immediately after PPA
//     (CameraTask can start the next capture while we run inference)
//   • Run ESPDet-Pico via ESP-DL in MULTI_CORE mode
//   • Apply confidence threshold and emit DetectionEvents to g_detection_queue
//
// Core pinning: Core 1 (APP_CPU), priority 5
// Stack: 12 KB (ESP-DL needs headroom for activation tensors on stack)
//
// Memory: inference_buf (224×224 RGB888 = ~147 KB) is static internal SRAM.
//         Model weights (~2 MB) are loaded to PSRAM at startup.
// ─────────────────────────────────────────────────────────────────────────────

#include "pipeline.h"
#include "trap_config.h"

class InferenceTask : public Task<12288> {
public:
    InferenceTask() = default;

    /// Start the inference task pinned to Core 1.
    void begin() { start("inference", 5, 1); }

    /// Number of detections emitted since startup.
    uint32_t detection_count() const { return detection_count_; }

protected:
    void run() override;

private:
    volatile uint32_t detection_count_ = 0;

    /// Static RGB888 buffer for PPA output — internal SRAM, never PSRAM.
    /// Size: FRAME_W × FRAME_H × 3 = 150,528 bytes.
    alignas(16) static uint8_t s_rgb_buf[FRAME_RGB888_BYTES];
};

extern InferenceTask g_inference_task;
