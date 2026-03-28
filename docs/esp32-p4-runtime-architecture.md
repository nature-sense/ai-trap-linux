# ESP32-P4 Trap Firmware — Runtime Architecture

**Language:** C++17 (no exceptions, no RTTI)
**RTOS:** FreeRTOS via ESP-IDF 5.x
**Target:** ESP32-P4NRW32, dual HP RISC-V cores @ 400 MHz + LP RISC-V core

---

## Design principles

1. **Zero-copy frame pipeline** — frame buffers are passed by pointer through queues; never
   memcpy'd between stages.
2. **Bounded queues everywhere** — all queues have a fixed depth. Producers drop frames rather
   than block indefinitely. The pipeline degrades gracefully under load instead of accumulating
   unbounded latency.
3. **Static allocation** — all tasks, queues, and semaphores use static FreeRTOS allocation
   (`StaticTask_t`, `StaticQueue_t`). No heap fragmentation; stack overflows are detectable at
   link time.
4. **Core isolation** — inference is pinned to Core 1 and never competes with camera ISRs or
   radio on Core 0.
5. **PM lock discipline** — `ESP_PM_CPU_FREQ_MAX` is held only during active processing;
   released while tasks are blocked on queues so the system can light-sleep between events.
6. **LP core owns sleep** — the LP RISC-V core monitors the PIR GPIO and wakes the HP cores.
   HP cores never call `esp_deep_sleep_start()` from within the pipeline; only the PowerTask
   does, after confirming the pipeline is drained.

---

## Core allocation

| Core | Role | FreeRTOS tasks |
|---|---|---|
| **Core 0** (PRO_CPU) | Drivers, I/O, radio | CameraTask, RadioTask, PowerTask, `app_main` |
| **Core 1** (APP_CPU) | Compute | InferenceTask |
| **LP Core** | Sleep watchdog | ULP program (PIR monitor), not a FreeRTOS core |

Inference is pinned to Core 1 so that `RUNTIME_MODE_MULTI_CORE` in ESP-DL spawns its helper
task onto Core 0 — the two HP cores share the workload for each Conv2D layer without
contending with each other on the same core.

---

## Task inventory

```
Priority (higher = more urgent; ESP-IDF idle = 0, timer daemon = 1)

Core 0:
  CameraTask      pri=6  stack=6 KB   pinned to Core 0
  RadioTask       pri=3  stack=8 KB   tskNO_AFFINITY (Core 0 preferred)
  PowerTask       pri=1  stack=4 KB   tskNO_AFFINITY

Core 1:
  InferenceTask   pri=5  stack=12 KB  pinned to Core 1
```

### CameraTask (Core 0, pri=6)

Owns the camera controller lifecycle and the frame buffer pool.

```
startup:
  allocate 2 × FrameBuffer in DMA-capable internal SRAM
  push both to buffer_pool_queue
  init CSI controller (esp_cam_new_csi_ctlr)
  register on_trans_finished ISR callback
  start controller

steady state:
  pop FrameBuffer* from buffer_pool_queue  ← blocks if no free buffers
  submit buffer to controller (esp_cam_ctlr_receive)
  [ISR fires on frame completion, see below]

ISR callback (on_trans_finished) — runs in ISR context:
  xQueueSendFromISR(frame_queue, &buf_ptr, &higher_pri_task_woken)
  if queue full: drop frame, increment drop_counter, do NOT block
  portYIELD_FROM_ISR(higher_pri_task_woken)
```

The ISR never blocks and never accesses PSRAM. Buffer pool backpressure is natural:
if InferenceTask hasn't returned the previous buffer, CameraTask blocks on
`buffer_pool_queue` before submitting the next capture — it cannot get ahead by
more than one inflight frame.

### InferenceTask (Core 1, pri=5)

The compute bottleneck. Runs continuously while frames are available.

```
startup:
  load ESPDet-Pico model from flash → PSRAM
  create PPA client (ppa_register_client, PPA_OPER_TYPE_SRM)
  acquire ESP_PM_CPU_FREQ_MAX lock (hold permanently while task is alive —
    release only when entering deep sleep)

steady state:
  xQueueReceive(frame_queue, &buf, portMAX_DELAY)  ← sleeps here between events

  // Stage 1: resize (hardware, non-blocking API used in blocking mode)
  ppa_srm_config.in  = buf->data          // YUV420, 224×224 capture
  ppa_srm_config.out = inference_buf      // RGB888, 224×224 (static, in SRAM)
  ppa_do_scale_rotate_mirror(ppa_client, &ppa_srm_config)  // blocking

  // Stage 2: return capture buffer to pool immediately after resize
  xQueueSend(buffer_pool_queue, &buf, 0)  // non-blocking; CameraTask can start next capture

  // Stage 3: inference
  input_tensor.set_element_ptr(inference_buf)
  model->run(&input_tensor, RUNTIME_MODE_MULTI_CORE)  // blocking ~55 ms

  // Stage 4: postprocess (NMS, threshold filter)
  detections = postprocess(model->get_outputs())

  // Stage 5: emit detection events
  for each detection above threshold:
    DetectionEvent evt = { .ts=esp_timer_get_time(), .class_id=..., .confidence=... }
    if xQueueSend(detection_queue, &evt, 0) fails: drop + log
```

The capture buffer is returned to the pool immediately after PPA completes (Stage 2),
before inference starts. This allows CameraTask to begin capturing the next frame while
inference runs on the resized copy in the static `inference_buf`. Because `inference_buf`
is in internal SRAM (not PSRAM), there is no PSRAM bandwidth contention between
PPA output and ESP-DL's weight fetches.

### RadioTask (Core 0, pri=3)

Transmits detection events over LoRa (SX1262 via SPI) or WiFi (via C6 addon).

```
startup:
  init radio driver (SX1262 SPI or ESP-Hosted)
  load any buffered events from NVS

steady state:
  xQueueReceive(detection_queue, &evt, RETRY_TIMEOUT_MS)

  if radio available:
    serialize evt to wire format (CBOR or compact binary, ~30 bytes)
    transmit (blocking SPI for LoRa; async socket for WiFi)
    if fail: nvs_write(evt)  ← buffer for later retry
  else:
    nvs_write(evt)

  periodically (every N events or M seconds):
    flush NVS buffer if radio now available
```

RadioTask runs at lower priority than InferenceTask so radio latency never stalls
the inference pipeline. The `detection_queue` depth (16 events) absorbs bursts.

### PowerTask (Core 0, pri=1)

Monitors idle time and orchestrates deep sleep entry.

```
steady state:
  wait IDLE_TIMEOUT_S with no new detections AND detection_queue empty

  // orderly shutdown
  signal CameraTask to stop accepting new captures
  wait for InferenceTask to drain (uxQueueMessagesWaiting(frame_queue) == 0)
  wait for RadioTask to drain (uxQueueMessagesWaiting(detection_queue) == 0)

  // hand off to LP core
  configure LP core ULP program (PIR GPIO wakeup)
  ulp_lp_core_run(&cfg)
  esp_sleep_enable_ulp_wakeup()
  esp_deep_sleep_start()  ← only called from here
```

---

## Queue inventory

All queues are statically allocated.

| Queue | Type | Depth | Item size | Producer | Consumer | Full policy |
|---|---|---|---|---|---|---|
| `frame_queue` | `FrameBuffer*` | 2 | 4 bytes (ptr) | Camera ISR | InferenceTask | Drop frame |
| `buffer_pool_queue` | `FrameBuffer*` | 2 | 4 bytes (ptr) | InferenceTask | CameraTask | Never full (depth = pool size) |
| `detection_queue` | `DetectionEvent` | 16 | ~32 bytes | InferenceTask | RadioTask | Drop oldest |

The `frame_queue` depth of 2 means InferenceTask can be at most 1 frame behind — if inference
takes longer than the capture interval, the oldest un-processed frame is dropped. For
ESPDet-Pico at ~55 ms and a capture interval of ~55 ms this is a soft constraint; if the
radio transmit causes brief Core 0 contention and the ISP misses a trigger, one frame is lost
silently.

---

## Memory layout

```
Internal SRAM (768 KB total — DMA-capable, zero-wait access)
┌─────────────────────────────────────────────┬────────┐
│ FreeRTOS kernel + OS overhead               │ ~80 KB │
│ Task stacks (4 tasks × ~8 KB avg)           │ ~32 KB │
│ Static TCBs + queue storage                 │  ~8 KB │
│ Frame buffer A  (224×224 YUV420, DMA)       │ ~75 KB │
│ Frame buffer B  (224×224 YUV420, DMA)       │ ~75 KB │
│ inference_buf   (224×224 RGB888, PPA dest)  │~150 KB │
│ ESP-DL hot activations (MALLOC_CAP_INTERNAL)│~100 KB │
│ Headroom / driver buffers                   │~248 KB │
└─────────────────────────────────────────────┴────────┘

PSRAM (32 MB on-package)
┌─────────────────────────────────────────────┬────────┐
│ ESPDet-Pico model weights (.espdl)          │  ~2 MB │
│ ESP-DL cold activations + work buffers      │  ~2 MB │
│ NVS event ring buffer                       │  ~1 MB │
│ Available for future use                    │ ~27 MB │
└─────────────────────────────────────────────┴────────┘
```

**DMA constraint:** `frame_queue` buffers and `inference_buf` are allocated with
`heap_caps_malloc(size, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)`.
PPA source and destination must be DMA-accessible; PSRAM cannot be used for these.

---

## Key C++ patterns

### RAII wrappers

```cpp
// PM lock — released automatically on scope exit or exception path
class PmLock {
    esp_pm_lock_handle_t handle_;
public:
    explicit PmLock(esp_pm_lock_type_t type, const char* name) {
        ESP_ERROR_CHECK(esp_pm_lock_create(type, 0, name, &handle_));
    }
    void acquire() { ESP_ERROR_CHECK(esp_pm_lock_acquire(handle_)); }
    void release() { ESP_ERROR_CHECK(esp_pm_lock_release(handle_)); }
    ~PmLock()      { esp_pm_lock_delete(handle_); }
};

// Frame buffer — DMA-capable internal SRAM, freed on destruction
class FrameBuffer {
public:
    uint8_t* data;
    size_t   size;
    explicit FrameBuffer(size_t sz)
        : data(static_cast<uint8_t*>(
              heap_caps_malloc(sz, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL))),
          size(sz) {
        assert(data != nullptr);
    }
    ~FrameBuffer() { heap_caps_free(data); }
    // non-copyable
    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;
};

// Typed static queue wrapper
template<typename T, size_t Depth>
class StaticQueue {
    StaticQueue_t   storage_;
    uint8_t         buf_[Depth * sizeof(T)];
    QueueHandle_t   handle_;
public:
    StaticQueue() {
        handle_ = xQueueCreateStatic(Depth, sizeof(T), buf_, &storage_);
        assert(handle_ != nullptr);
    }
    bool send(const T& item, TickType_t wait = 0) {
        return xQueueSend(handle_, &item, wait) == pdTRUE;
    }
    bool send_from_isr(const T& item, BaseType_t* woken) {
        return xQueueSendFromISR(handle_, &item, woken) == pdTRUE;
    }
    bool receive(T& item, TickType_t wait = portMAX_DELAY) {
        return xQueueReceive(handle_, &item, wait) == pdTRUE;
    }
    size_t waiting() const { return uxQueueMessagesWaiting(handle_); }
};

// Task base — static stack + TCB
template<size_t StackBytes>
class Task {
    StaticTask_t    tcb_;
    StackType_t     stack_[StackBytes / sizeof(StackType_t)];
    TaskHandle_t    handle_ = nullptr;
protected:
    virtual void run() = 0;
public:
    void start(const char* name, UBaseType_t priority, BaseType_t core) {
        handle_ = xTaskCreateStaticPinnedToCore(
            [](void* arg) { static_cast<Task*>(arg)->run(); },
            name, StackBytes, this, priority, stack_, &tcb_, core);
        assert(handle_ != nullptr);
    }
};
```

### Detection event

```cpp
struct DetectionEvent {
    int64_t  timestamp_us;   // esp_timer_get_time()
    uint8_t  class_id;       // species index
    uint8_t  confidence;     // 0–100 (scaled from float)
    uint16_t trap_id;        // configured at flash time
    // 8 bytes — fits comfortably in a LoRa payload alongside metadata
};
static_assert(sizeof(DetectionEvent) == 8);
```

---

## Power state machine

```
          PIR triggers LP core
                  │
    ┌─────────────▼──────────────┐
    │         DEEP SLEEP         │ LP core active, HP cores off
    │   LP monitors PIR GPIO     │ ~1–5 mA total
    └─────────────┬──────────────┘
                  │ ulp_lp_core_wakeup_main_processor()
    ┌─────────────▼──────────────┐
    │           WAKING           │ HP boot, peripheral init
    │  ~10–50 ms, one-time cost  │ ~100–200 mA
    └─────────────┬──────────────┘
                  │ pipeline ready
    ┌─────────────▼──────────────┐
    │           RUNNING          │ Camera + inference loop
    │  ESP_PM_CPU_FREQ_MAX held  │ ~200–400 mA during inference
    │  light-sleep between frames│ ~10–20 mA between frames
    └──────┬──────────────┬──────┘
           │ detection    │ no detection for IDLE_TIMEOUT_S
           │              │
    ┌──────▼──────┐  ┌────▼────────────────────────────────┐
    │ TRANSMIT    │  │             DRAINING                 │
    │ ~40 mA LoRa │  │ wait for queues to empty, radio done │
    │ ~250mA WiFi │  └────────────────┬────────────────────┘
    └──────┬──────┘                   │
           │ ack / timeout            │ drained
           └──────────┬───────────────┘
                       │
             back to RUNNING or
             (if idle) to DEEP SLEEP
```

---

## LP core ULP program outline

```c
// ulp_pir_monitor.c  — compiled for LP RISC-V core
#include "ulp_lp_core.h"
#include "ulp_lp_core_lp_io_ana_cmds.h"

void LP_CORE_ISR_ATTR ulp_lp_core_lp_io_intr_handler(void) {
    // Debounce: ignore if HP cores recently woke (avoid rapid re-trigger)
    // Check PIR GPIO level (high = motion detected)
    if (lp_io_get_level(PIR_GPIO_NUM) == 1) {
        ulp_lp_core_wakeup_main_processor();
    }
    // re-arm interrupt
}

int main(void) {
    // Configure LP GPIO interrupt on PIR pin
    lp_io_set_dir(PIR_GPIO_NUM, LP_IO_MODE_INPUT);
    lp_io_int_configure(PIR_GPIO_NUM, LP_IO_INTR_POSEDGE);
    lp_io_int_enable(PIR_GPIO_NUM);
    ulp_lp_core_intr_enable();
    // LP core sleeps here, wakes on GPIO interrupt
    while (1) {
        ulp_lp_core_halt();
    }
}
```

---

## Startup sequence

```
app_main() — Core 0
  1. nvs_flash_init()
  2. esp_pm_configure() — set max/min freq, enable auto light-sleep
  3. Determine wakeup reason (esp_sleep_get_wakeup_cause())
     - First boot: full init
     - ULP wakeup: skip NVS erase, restore state
  4. Create all static queues and frame buffers
  5. Load ESPDet-Pico model into PSRAM
  6. Create and start tasks (PowerTask last)
  7. app_main returns — FreeRTOS scheduler takes over
```

---

## Error handling policy

| Error | Response |
|---|---|
| Frame drop (queue full) | Increment counter; log at 10 Hz max; no action |
| PPA error | Log; skip frame; return buffer to pool |
| Inference error | Log; skip frame; continue |
| `detection_queue` full | Drop oldest event; log overflow count |
| Radio TX failure | Write event to NVS ring buffer; retry on next cycle |
| NVS full | Overwrite oldest entry (ring buffer semantics) |
| Stack overflow (if `configCHECK_FOR_STACK_OVERFLOW=2`) | `vApplicationStackOverflowHook` → log + restart |
| Heap exhaustion | Not expected with static allocation; `assert` in debug builds |

---

## What this architecture does NOT include (future phases)

- **OTA firmware update** — trivially difficult over LoRa; use WiFi path when near a field
  station, or manual reflash via USB. Phase 3 concern.
- **RTC timekeeping** — timestamps use `esp_timer_get_time()` (microseconds since boot).
  Real wall-clock time requires GPS pulse or NTP over WiFi at wakeup.
- **Multi-species model** — ESPDet-Pico is single-class. Multiple classes require either a
  larger model (latency impact) or multiple sequential inference passes.
- **Image sampling** — 1-in-N JPEG capture for QC. Straightforward addition: if
  `(detection_count % SAMPLE_RATE == 0)`, encode the PPA output with the hardware JPEG
  encoder and queue for upload.

---

## Related documents

- [esp32-p4-ai-evaluation.md](esp32-p4-ai-evaluation.md) — Hardware and AI benchmark details
- [esp32-p4-insect-trap-architecture.md](esp32-p4-insect-trap-architecture.md) — System-level
  architecture, network options, experimental roadmap
