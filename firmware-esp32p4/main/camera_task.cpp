// ─────────────────────────────────────────────────────────────────────────────
// camera_task.cpp — CameraTask implementation
// ─────────────────────────────────────────────────────────────────────────────

#include "camera_task.h"
#include "pipeline.h"
#include "trap_config.h"

#include "esp_cam_ctlr_csi.h"   // P4 CSI controller API (ESP-IDF 5.3+)
#include "esp_cam_ctlr.h"
#include "driver/isp.h"         // P4 hardware ISP
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "camera";

CameraTask g_camera_task;

// ── ISR callback ─────────────────────────────────────────────────────────────
// Runs in interrupt context — absolutely no blocking, no PSRAM access.
// Sends the completed buffer pointer to g_frame_queue; drops on overflow.

bool CameraTask::on_frame_ready(
    esp_cam_ctlr_handle_t /*handle*/,
    esp_cam_ctlr_trans_t* /*trans*/,
    void* user_ctx)
{
    BaseType_t woken = pdFALSE;
    // Retrieve the FrameBuffer* stored before the receive call via user_ctx.
    auto* self = static_cast<CameraTask*>(user_ctx);
    auto* buf  = self->pending_buf_;

    if (!g_frame_queue.send_from_isr(buf, &woken)) {
        // InferenceTask not keeping up — drop this frame, return buffer to pool.
        g_frame_drop_count++;
        g_buffer_pool_queue.send_from_isr(buf, &woken);
    }

    portYIELD_FROM_ISR(woken);
    return true; // return true = continue capturing
}

// ── CameraTask::run() ─────────────────────────────────────────────────────────

void CameraTask::run() {
    ESP_LOGI(TAG, "Starting camera task");

    // ── 1. OV5647 sensor init ─────────────────────────────────────────────
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(ov5647_init(&i2c_bus));
    ESP_ERROR_CHECK(ov5647_check_id(i2c_bus));
    ESP_ERROR_CHECK(ov5647_configure(i2c_bus));

    // ── 2. Allocate frame buffer pool ─────────────────────────────────────
    // Two DMA buffers in internal SRAM (ping-pong).
    // Pool starts full: both buffers available for CameraTask to submit.
    static FrameBuffer buf_a(FRAME_YUV420_BYTES);
    static FrameBuffer buf_b(FRAME_YUV420_BYTES);

    FrameBuffer* bufs[2] = {&buf_a, &buf_b};
    for (auto* b : bufs) {
        // Pre-populate pool with all free buffers
        ESP_ERROR_CHECK(g_buffer_pool_queue.send(b, portMAX_DELAY)
            ? ESP_OK : ESP_FAIL);
    }

    // ── 3. Configure P4 CSI controller ───────────────────────────────────
    // OV5647 outputs 640×480 RAW10 on MIPI CSI-2 1-lane.
    // The P4 ISP receives RAW10, applies AWB/AEC/demosaic, and outputs
    // YUV420 at FRAME_W × FRAME_H = 224×224 (centre-crop + scale).
    esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .clk_src                = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res                  = FRAME_W,       // 224 — ISP output width
        .v_res                  = FRAME_H,       // 224 — ISP output height
        .data_lane_num          = 1,             // OV5647: single MIPI lane
        .lane_bit_rate_mbps     = 200,           // OV5647 MIPI 1-lane @ 200 Mbps
        .input_data_color_type  = CAM_CTLR_COLOR_RAW10,  // from OV5647
        .output_data_color_type = CAM_CTLR_COLOR_YUV420, // to DMA buffer
        .queue_items            = 2,             // match pool depth
        .byte_swap_en           = false,
    };

    esp_cam_ctlr_handle_t cam;
    ESP_ERROR_CHECK(esp_cam_new_csi_ctlr(&csi_cfg, &cam));

    // Register ISR callback
    esp_cam_ctlr_evt_cbs_t cbs = {
        .on_trans_finished = CameraTask::on_frame_ready,
    };
    ESP_ERROR_CHECK(esp_cam_ctlr_register_event_callbacks(cam, &cbs, this));
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(cam));
    ESP_ERROR_CHECK(esp_cam_ctlr_start(cam));

    ESP_LOGI(TAG, "CSI controller started — %u×%u YUV420", FRAME_W, FRAME_H);

    // ── 4. Frame submission loop ──────────────────────────────────────────
    // Pull a free buffer from the pool, submit it to the controller.
    // The ISR will fire when the frame is complete and push the buffer
    // to g_frame_queue for InferenceTask.
    while (!stop_requested_) {
        FrameBuffer* buf = nullptr;

        // Block until a buffer is returned from InferenceTask (after PPA).
        // If neither buffer is free the camera waits — natural backpressure.
        if (!g_buffer_pool_queue.receive(buf, pdMS_TO_TICKS(500))) {
            // Timeout — check stop flag and retry
            continue;
        }

        // Store current buffer so the ISR can retrieve it via user_ctx.
        pending_buf_ = buf;
        esp_cam_ctlr_trans_t trans = {
            .buffer = buf->data,
            .buflen = buf->size,
        };
        esp_err_t err = esp_cam_ctlr_receive(cam, &trans, pdMS_TO_TICKS(200));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ctlr_receive error %s — returning buffer", esp_err_to_name(err));
            g_buffer_pool_queue.send(buf, 0);
        }
    }

    // ── 5. Orderly shutdown ───────────────────────────────────────────────
    ESP_LOGI(TAG, "Stopping — sensor standby + CSI shutdown");
    ov5647_standby(i2c_bus);
    esp_cam_ctlr_stop(cam);
    esp_cam_ctlr_disable(cam);
    esp_cam_ctlr_del(cam);
    ov5647_power_down();
}
