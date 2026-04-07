#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// pipeline.h — Shared types, RAII wrappers, and inter-task queues
//
// Design rules (enforced by code):
//   • All queues are statically allocated — no heap fragmentation.
//   • All tasks use static TCBs and stacks — stack overflows are link-time
//     errors, not runtime surprises.
//   • Frame buffers are DMA-capable internal SRAM only (PSRAM is not
//     DMA-accessible and must never be used for frame or PPA buffers).
//   • Zero-copy: frames are passed as pointers; never memcpy'd.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstddef>
#include <cassert>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

// ── Capture / inference dimensions ───────────────────────────────────────────

/// Camera capture and inference input resolution.
/// OV5647 outputs 640×480 via MIPI; the P4 ISP crops and scales to this size.
/// 224×224 YUV420 = 224 × 224 × 1.5 = 75,264 bytes per buffer — fits in SRAM.
static constexpr uint32_t FRAME_W = 224;
static constexpr uint32_t FRAME_H = 224;

/// YUV420 planar: Y plane (W×H) + UV plane (W×H/2)
static constexpr size_t FRAME_YUV420_BYTES = FRAME_W * FRAME_H * 3 / 2; // 75,264

/// RGB888 inference input: W × H × 3 bytes
static constexpr size_t FRAME_RGB888_BYTES = FRAME_W * FRAME_H * 3;     // 150,528

// ── FrameBuffer ───────────────────────────────────────────────────────────────

/// DMA-capable frame buffer allocated in internal SRAM.
/// Non-copyable; passed by pointer through the pipeline.
class FrameBuffer {
public:
    uint8_t* const data;
    const size_t   size;

    explicit FrameBuffer(size_t bytes)
        : data(static_cast<uint8_t*>(
              heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))),
          size(bytes)
    {
        assert(data != nullptr && "FrameBuffer DMA allocation failed — check SRAM budget");
    }

    ~FrameBuffer() { heap_caps_free(data); }

    // Non-copyable, non-movable
    FrameBuffer(const FrameBuffer&)            = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;
    FrameBuffer(FrameBuffer&&)                 = delete;
    FrameBuffer& operator=(FrameBuffer&&)      = delete;
};

// ── DetectionEvent ────────────────────────────────────────────────────────────

/// A single confirmed detection event passed from InferenceTask → RadioTask.
/// Kept to 8 bytes so it fits in a compact LoRa payload alongside metadata.
struct DetectionEvent {
    int64_t  timestamp_us;  ///< esp_timer_get_time() at detection
    uint8_t  class_id;      ///< Species index (0 = insect, single-class model)
    uint8_t  confidence;    ///< Scaled 0–100 from model float confidence
    uint16_t trap_id;       ///< Configured at flash time in TrapConfig
};
static_assert(sizeof(DetectionEvent) == 16,
    "DetectionEvent layout changed — check wire format");
// Note: int64_t has 8-byte alignment on RV32 RISC-V, so the struct is 16 bytes
// (8 + 1 + 1 + 2 + 4 bytes padding). The LoRa payload encodes fields explicitly.

// ── StaticQueue ───────────────────────────────────────────────────────────────

/// Typed wrapper around FreeRTOS xQueueCreateStatic.
/// All storage is embedded in the struct — no heap allocation.
template<typename T, size_t Depth>
class StaticQueue {
    StaticQueue_t storage_;
    uint8_t       buf_[Depth * sizeof(T)];
    QueueHandle_t handle_;

public:
    StaticQueue() {
        handle_ = xQueueCreateStatic(Depth, sizeof(T), buf_, &storage_);
        assert(handle_ != nullptr);
    }

    /// Send from a task context. Returns true on success.
    /// wait = 0 → non-blocking drop; portMAX_DELAY → block until space.
    bool send(const T& item, TickType_t wait = 0) {
        return xQueueSend(handle_, &item, wait) == pdTRUE;
    }

    /// Send from an ISR. Sets *woken if a higher-priority task was unblocked.
    bool send_from_isr(const T& item, BaseType_t* woken) {
        return xQueueSendFromISR(handle_, &item, woken) == pdTRUE;
    }

    /// Receive. Default blocks forever (task will sleep here between events).
    bool receive(T& item, TickType_t wait = portMAX_DELAY) {
        return xQueueReceive(handle_, &item, wait) == pdTRUE;
    }

    size_t waiting() const { return uxQueueMessagesWaiting(handle_); }
    size_t spaces()  const { return uxQueueSpacesAvailable(handle_); }

    QueueHandle_t handle() { return handle_; }
};

// ── Task base ─────────────────────────────────────────────────────────────────

/// Statically allocated task with embedded stack and TCB.
/// Subclass and override run().
template<size_t StackBytes>
class Task {
    StaticTask_t tcb_;
    StackType_t  stack_[StackBytes / sizeof(StackType_t)];
    TaskHandle_t handle_ = nullptr;

    static void entry(void* arg) {
        static_cast<Task*>(arg)->run();
        // Tasks must not return — loop forever or delete self.
        vTaskDelete(nullptr);
    }

protected:
    virtual void run() = 0;

public:
    virtual ~Task() = default;

    void start(const char* name, UBaseType_t priority, BaseType_t core) {
        handle_ = xTaskCreateStaticPinnedToCore(
            entry, name, StackBytes, this,
            priority, stack_, &tcb_, core);
        assert(handle_ != nullptr);
    }

    TaskHandle_t handle() const { return handle_; }

    /// Notify this task from another task or ISR.
    void notify_from_isr(BaseType_t* woken) {
        vTaskNotifyGiveFromISR(handle_, woken);
    }
    void notify() { xTaskNotifyGive(handle_); }
};

// ── PmLock ─────────────────────────────────────────────────────────────────────

#include "esp_pm.h"

/// RAII wrapper for esp_pm_lock. Prevents CPU from throttling while held.
class PmLock {
    esp_pm_lock_handle_t handle_;
public:
    explicit PmLock(esp_pm_lock_type_t type, const char* name) {
        ESP_ERROR_CHECK(esp_pm_lock_create(type, 0, name, &handle_));
    }
    ~PmLock() { esp_pm_lock_delete(handle_); }

    void acquire() { ESP_ERROR_CHECK(esp_pm_lock_acquire(handle_)); }
    void release() { ESP_ERROR_CHECK(esp_pm_lock_release(handle_)); }

    PmLock(const PmLock&)            = delete;
    PmLock& operator=(const PmLock&) = delete;
};

// ── Global inter-task queues ──────────────────────────────────────────────────
//
// Defined once in app_main.cpp; declared extern here so tasks can access them.
//
//   frame_queue        Camera ISR → InferenceTask      depth=2  item=FrameBuffer*
//   buffer_pool_queue  InferenceTask → CameraTask       depth=2  item=FrameBuffer*
//   detection_queue    InferenceTask → RadioTask        depth=16 item=DetectionEvent
//
// See runtime-architecture.md for full queue design rationale.

extern StaticQueue<FrameBuffer*, 2>     g_frame_queue;
extern StaticQueue<FrameBuffer*, 2>     g_buffer_pool_queue;
extern StaticQueue<DetectionEvent, 16>  g_detection_queue;

/// Monotonic count of frames dropped because InferenceTask was too slow.
extern volatile uint32_t g_frame_drop_count;

/// Monotonic count of detection events dropped because RadioTask was busy.
extern volatile uint32_t g_event_drop_count;

/// Set by PowerTask when requesting orderly shutdown before deep sleep.
extern volatile bool g_shutdown_requested;
