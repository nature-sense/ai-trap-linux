#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// camera_task.h — CameraTask: owns the CSI controller and frame buffer pool
//
// Responsibilities:
//   • Initialise the OV5647 sensor over SCCB (I2C)
//   • Set up the ESP32-P4 CSI controller and ISP pipeline
//   • Manage a pool of two DMA frame buffers (ping-pong)
//   • Feed completed frames into g_frame_queue for InferenceTask
//   • Drop frames (not block) when InferenceTask is busy — frame_queue depth=2
//
// Core pinning: Core 0 (PRO_CPU), priority 6
// Stack: 6 KB
//
// ISR: on_frame_ready runs in ISR context — must never block or access PSRAM.
// ─────────────────────────────────────────────────────────────────────────────

#include "pipeline.h"
#include "ov5647.h"
#include "esp_cam_ctlr.h"

class CameraTask : public Task<6144> {
public:
    explicit CameraTask() = default;

    /// Start the camera task pinned to Core 0.
    void begin() { start("camera", 6, 0); }

    /// Signal the task to stop accepting new captures (called by PowerTask).
    void request_stop() { stop_requested_ = true; }

protected:
    void run() override;

private:
    volatile bool   stop_requested_ = false;
    FrameBuffer*    pending_buf_     = nullptr; ///< Buffer currently being captured

    static bool on_frame_ready(
        esp_cam_ctlr_handle_t handle,
        esp_cam_ctlr_trans_t* trans,
        void* user_ctx);
};

extern CameraTask g_camera_task;
