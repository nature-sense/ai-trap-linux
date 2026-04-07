// ─────────────────────────────────────────────────────────────────────────────
// inference_task.cpp — InferenceTask implementation
//
// Model: ESPDet-Pico (224×224 INT8, ~0.36 M params)
//   Stored in the "model" flash partition (see partitions.csv).
//   Loaded to PSRAM at startup via ESP-DL.
//   Runs in MULTI_CORE mode so both HP cores share the Conv2D workload.
//
// ESP-DL version: ≥2.0.0 (FlatBuffers .espdl format)
//   If ESPDet-Pico is not available in the partition, the task logs an error
//   and loops — it does not crash the system.
// ─────────────────────────────────────────────────────────────────────────────

#include "inference_task.h"
#include "pipeline.h"
#include "trap_config.h"

// ESP-DL headers (managed component: espressif/esp-dl ≥2.0.0)
#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

// PPA (Pixel Processing Accelerator) — P4 hardware pixel converter
#include "driver/ppa.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "inference";

InferenceTask g_inference_task;

// Static RGB888 inference buffer — internal SRAM, 16-byte aligned for PPA DMA
alignas(16) uint8_t InferenceTask::s_rgb_buf[FRAME_RGB888_BYTES];

// ── Model output postprocessing ───────────────────────────────────────────────
// ESPDet-Pico output format (after ESP-DL internal NMS):
//   Tensor name: "output"
//   Shape: [1, N, 6]  where N = max_detections (model-dependent, typically 100)
//   Layout per row: [x1, y1, x2, y2, confidence, class_id]  — normalised [0,1]
//
// We iterate N rows, filter by confidence threshold, and emit DetectionEvents.

static void postprocess_and_emit(
    const dl::TensorBase* output,
    float conf_threshold,
    uint16_t trap_id)
{
    if (!output) {
        ESP_LOGW(TAG, "Null output tensor");
        return;
    }

    // Expect shape [1, N, 6]
    const auto& shape = output->shape;
    if (shape.size() != 3 || shape[2] < 6) {
        ESP_LOGW(TAG, "Unexpected output shape: ndim=%zu", shape.size());
        return;
    }

    const int num_dets = shape[1];
    const float* data  = static_cast<const float*>(output->data);
    const int64_t now  = esp_timer_get_time();

    for (int i = 0; i < num_dets; ++i) {
        const float* row = data + i * shape[2];
        const float  conf = row[4];

        if (conf < conf_threshold) continue;

        // Emit detection event — scale confidence to 0-100 uint8
        DetectionEvent evt = {
            .timestamp_us = now,
            .class_id     = static_cast<uint8_t>(row[5]),
            .confidence   = static_cast<uint8_t>(conf * 100.f),
            .trap_id      = trap_id,
        };

        if (!g_detection_queue.send(evt, 0)) {
            // RadioTask queue full — drop event
            g_event_drop_count++;
            ESP_LOGD(TAG, "detection_queue full — event dropped (total drops: %lu)",
                     (unsigned long)g_event_drop_count);
        }
    }
}

// ── InferenceTask::run() ───────────────────────────────────────────────────────

void InferenceTask::run() {
    ESP_LOGI(TAG, "Starting inference task");

    // ── 1. Load ESPDet-Pico model from flash partition to PSRAM ──────────
    // The "model" partition (4 MB) must be flashed with the .espdl file.
    //   idf.py -p PORT flash --partition-table-offset 0x9000
    //   esptool.py write_flash 0x290000 espdet_pico_224_int8.espdl
    //
    // ESP-DL loads weights into PSRAM automatically (MALLOC_CAP_SPIRAM).
    // Hot activations are placed in internal SRAM by ESP-DL's memory planner.

    dl::Model* model = nullptr;
    {
        ESP_LOGI(TAG, "Loading ESPDet-Pico from flash partition 'model'…");
        model = new dl::Model(
            "model",                              // partition label
            fbs::MODEL_LOCATION_IN_FLASH_PARTITION
            // max_internal_size=0: let esp-dl use PSRAM for weights
            // MEMORY_MANAGER_GREEDY is the default (only supported option)
        );
        if (!model) {
            ESP_LOGE(TAG, "Failed to load model — inference disabled, task halted");
            vTaskDelete(nullptr);
            return;
        }
        ESP_LOGI(TAG, "ESPDet-Pico loaded OK");
    }

    // ── 2. Create PPA client for YUV420 → RGB888 conversion ──────────────
    // PPA runs in hardware on the P4 at zero CPU cost.
    // Source: YUV420 semi-planar (NV12), 224×224, in DMA SRAM frame buffer.
    // Dest:   RGB888, 224×224, in s_rgb_buf (internal SRAM).

    ppa_client_handle_t ppa_client = nullptr;
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,   // Scale-Rotate-Mirror (also handles colour conversion)
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ppa_client));

    // ── 3. Inference loop ─────────────────────────────────────────────────
    ESP_LOGI(TAG, "Entering inference loop");

    while (true) {
        // Block until CameraTask delivers a frame.
        // Between frames the task blocks here and the system can light-sleep.
        FrameBuffer* buf = nullptr;
        if (!g_frame_queue.receive(buf, portMAX_DELAY)) continue;

        const int64_t t0 = esp_timer_get_time();

        // ── Stage 1: PPA — YUV420 → RGB888 ───────────────────────────────
        ppa_srm_oper_config_t srm = {};

        // Source: YUV420 NV12 planar in frame buffer (DMA SRAM)
        srm.in.buffer      = buf->data;
        srm.in.pic_w       = FRAME_W;
        srm.in.pic_h       = FRAME_H;
        srm.in.block_w     = FRAME_W;
        srm.in.block_h     = FRAME_H;
        srm.in.block_offset_x = 0;
        srm.in.block_offset_y = 0;
        srm.in.srm_cm      = PPA_SRM_COLOR_MODE_YUV420;

        // Destination: RGB888 in s_rgb_buf (static internal SRAM)
        srm.out.buffer      = s_rgb_buf;
        srm.out.buffer_size = FRAME_RGB888_BYTES;
        srm.out.pic_w       = FRAME_W;
        srm.out.pic_h       = FRAME_H;
        srm.out.block_offset_x = 0;
        srm.out.block_offset_y = 0;
        srm.out.srm_cm      = PPA_SRM_COLOR_MODE_RGB888;

        srm.rotation_angle  = PPA_SRM_ROTATION_ANGLE_0;
        srm.scale_x         = 1.0f;
        srm.scale_y         = 1.0f;
        srm.mirror_x        = false;
        srm.mirror_y        = false;
        srm.mode            = PPA_TRANS_MODE_BLOCKING;

        esp_err_t ppa_err = ppa_do_scale_rotate_mirror(ppa_client, &srm);

        // ── Stage 2: Return capture buffer immediately after PPA ──────────
        // CameraTask can start the next capture while inference runs on the
        // static s_rgb_buf copy. Critical for pipeline throughput.
        g_buffer_pool_queue.send(buf, 0);
        buf = nullptr; // no longer valid to use

        if (ppa_err != ESP_OK) {
            ESP_LOGW(TAG, "PPA error %s — skipping frame", esp_err_to_name(ppa_err));
            continue;
        }

        // ── Stage 3: Build input tensor and run ESPDet-Pico ──────────────
        // ESP-DL tensor wraps s_rgb_buf in-place — no copy.
        // Input: [1, H, W, 3] uint8 RGB888, normalised inside the model.
        dl::TensorBase input_tensor(
            {1, static_cast<int>(FRAME_H), static_cast<int>(FRAME_W), 3},
            s_rgb_buf,
            0,         // exponent (raw uint8)
            dl::DATA_TYPE_UINT8
        );

        // Inference — blocks ~55 ms on ESP32-P4 with ESPDet-Pico
        model->run(&input_tensor);

        const int64_t t1 = esp_timer_get_time();

        // ── Stage 4: Postprocess and emit detection events ────────────────
        auto outputs = model->get_outputs();
        const dl::TensorBase* out_tensor = nullptr;
        if (!outputs.empty()) {
            out_tensor = outputs.begin()->second; // first (only) output
        }

        postprocess_and_emit(out_tensor, g_config.conf_threshold, g_config.trap_id);

        const int64_t t2 = esp_timer_get_time();

        ESP_LOGD(TAG, "Frame done: PPA %lld µs, infer %lld µs, post %lld µs",
                 (t1 - t0), (t2 - t1), (esp_timer_get_time() - t2));

        detection_count_++;

        // Check shutdown request from PowerTask
        if (g_shutdown_requested) {
            ESP_LOGI(TAG, "Shutdown requested — draining and exiting");
            break;
        }
    }

    // Cleanup
    ppa_unregister_client(ppa_client);
    delete model;
    ESP_LOGI(TAG, "Inference task exiting (total frames: %lu)", (unsigned long)detection_count_);
}
