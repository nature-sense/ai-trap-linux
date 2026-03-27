# ESP32-P4NRW32 — AI Capabilities Evaluation

**Date:** March 2026
**Part:** Espressif ESP32-P4NRW32
**Purpose:** Evaluate suitability for wildlife trap camera inference workload (YOLOv11n 320×320)

---

## Part number decoded

| Field | Meaning |
|---|---|
| ESP32-P4 | P4 SoC family |
| N | No embedded flash (external flash required) |
| R | Embedded PSRAM present |
| W | P4 variant code (P4 has no Wi-Fi regardless) |
| 32 | 32 MB on-package PSRAM |

---

## Hardware specifications

| Parameter | Value |
|---|---|
| HP CPU | Dual-core 32-bit RISC-V @ **400 MHz** |
| LP CPU | Single-core RISC-V @ 40 MHz (idle/wake management) |
| On-chip SRAM | **768 KB** + 8 KB zero-wait TCM |
| PSRAM (NRW32) | **32 MB** on-package, octal SPI |
| FPU | float32 hardware |
| Wireless | **None** — companion chip required for Wi-Fi/BLE |
| Package | QFN-104, 10×10 mm |

---

## AI acceleration hardware

There is **no discrete NPU**. Acceleration is provided by the **PIE/Xai custom ISA extension** baked
into the RISC-V cores:

- **16 parallel INT8 MACs per cycle** via 8×128-bit vector (Q-register) units
- **256-bit QACC accumulator** — wider than the ESP32-S3's 160-bit, meaningful for conv layers
- Both HP cores can execute PIE instructions; ESP-DL auto-schedules Conv2D across both cores
- Parallel load/store: full 128-bit Q-register in a single cycle (data must be 128-bit aligned)

This is not a separate hardware block — it is an ISA extension in the main CPU pipeline. All inference
runs on the general-purpose cores; peak throughput is bounded by the 16×INT8 MAC/cycle ceiling.

Additional vision-relevant hardware blocks:

- **MIPI CSI-2 ISP**: hardware image signal processor up to 1080p (noise reduction, demosaicing, AWB, AEC)
- **PPA (Pixel Processing Accelerator)**: hardware resize/rotate/crop — decouples capture resolution from inference input resolution
- **H.264 encoder**: hardware, 1080p @ 30 fps

---

## YOLOv11n at 320×320 — official benchmark

Espressif publishes benchmark data for the pre-quantized INT8 model `yolo11n_320_s8_v3_p4`:

| Stage | Latency |
|---|---|
| Preprocessing | 5.6 ms |
| Model inference | 600.0 ms |
| Postprocessing | 5.8 ms |
| **Total** | **~611 ms (~1.6 FPS)** |
| mAP50-95 (COCO 80-class) | 27.5% |

The 640×640 variant achieves mAP50-95 of 36.0% at ~2.76 s total (~0.36 FPS).

For a **triggered trap camera** (one inference pass per motion event) 1.6 FPS is workable.
The PPA can resize 1080p → 320×320 in hardware at no CPU cost.

For higher throughput, Espressif's **ESPDet-Pico** (224×224, 0.36 M params) reaches **>18 FPS**,
well suited to a single-species fine-tuned detector.

---

## Supported AI frameworks

| Framework | Status |
|---|---|
| **ESP-DL** | Primary/recommended. Native `.espdl` FlatBuffers format, full PIE acceleration |
| **TensorFlow Lite Micro** | Supported via `espressif/esp-tflite-micro` |
| **ONNX** | Input to ESP-PPQ quantization pipeline only — cannot deploy ONNX Runtime directly |
| **PyTorch / TensorFlow** | Export to ONNX → quantize with ESP-PPQ → deploy as `.espdl` |

**ESP-PPQ** (based on PPQ) handles INT8 post-training quantization and quantization-aware training
from ONNX/PyTorch/TensorFlow to the `.espdl` model format.

---

## Camera interface

| Parameter | Value |
|---|---|
| MIPI CSI-2 | DPHY v1.1, 2-lane × 1.5 Gbps = 3 Gbps total |
| ISP max resolution | 1920×1080 |
| ISP output formats | RAW8, RGB888, RGB565, YUV422, YUV420 |
| H.264 / JPEG | Hardware, 1080p @ 30 fps |
| DVP parallel | Also supported (max ~1 MP for YUV/RGB) |
| PPA | Hardware scale, rotate, mirror — resize frames to inference resolution at zero CPU cost |

---

## Available AI examples and demos

- **[esp-dl](https://github.com/espressif/esp-dl)** — Core library. Pre-quantized YOLOv11n, face
  detection/recognition, MNIST, COCO detection model family, YOLOv26 (added March 2026)
- **[esp-detection](https://github.com/espressif/esp-detection)** — Lightweight real-time detection
  based on Ultralytics YOLOv11, with ESPDet-Pico training APIs for custom single-class models
- **[esp-tflite-micro](https://github.com/espressif/esp-tflite-micro)** — TFLite Micro examples
  (wake word, person detection, gesture)
- **[esp-ppq](https://github.com/espressif/esp-ppq)** — Quantization toolchain with Netron
  visualization support
- **ESP32-P4-EYE** — Vision-focused development kit with MIPI-CSI camera and USB 2.0 HS

---

## Comparison with existing trap platforms

| | Luckfox Pico Zero | CM5 (Cortex-A55) | ESP32-P4NRW32 |
|---|---|---|---|
| CPU | Cortex-A7 @ 1.2 GHz | Cortex-A55 @ 2.4 GHz | RISC-V @ 400 MHz |
| AI accelerator | RKNN NPU (0.5 TOPS) | ARM NPU | PIE/SIMD (no NPU) |
| YOLOv11n 320×320 | ~300–500 ms | ~50 ms | ~611 ms |
| Wireless | AIC8800DC (built-in) | Built-in | **None — companion required** |
| Power (active) | ~0.5 W | ~4–8 W | ~0.5–1 W (estimated) |
| H.264 encode | No | Yes | Yes |
| Hardware ISP | No | Yes | Yes |
| OS | Linux (busybox/uClibc) | Linux (full Debian) | FreeRTOS (ESP-IDF) |
| Module price (approx.) | ~$7 | ~$60 | ~$4.52 (die only) |

---

## Known limitations and gotchas

### 1. No wireless on-chip
The P4 has zero Wi-Fi or Bluetooth. Any wireless upload of detections requires a companion chip
(e.g. ESP32-C6 via ESP-Hosted or ESP-AT). This adds BOM cost, PCB complexity, and inter-chip latency.

### 2. Silicon revision supply chain chaos (March 2026)
P4 v1.3 changed a pin vs. v1.0 and requires additional passive components. Both revisions ship
under the same `ESP32-P4NRW32` part number. Firmware must be compiled separately per revision;
boards designed for v1.0 may receive v1.3 chips from distributors without warning.
See: [Hackaday — ESP32: When Is A P4 A P4](https://hackaday.com/2026/03/21/esp32-when-is-a-p4-a-p4-but-not-the-p4-you-thought-it-was/)

### 3. DMA memory is internal SRAM only
`MALLOC_CAP_DMA` allocations cannot use PSRAM. Camera DMA frame buffers must fit in 768 KB internal
SRAM. A 320×320 RGB888 frame is ~300 KB — tight but manageable with careful allocation; double
buffering at that resolution is not straightforward.

### 4. PSRAM latency
Model weights reside in PSRAM and cause cache misses on access. Internal SRAM is used for hot
activations. ESP-DL's memory planner optimises layer placement, but PSRAM bandwidth is a real
bottleneck for large models.

### 5. No dedicated NPU
Unlike the Luckfox (RKNN) or ARM Cortex-M55+Ethos-U, all inference runs on the general-purpose
RISC-V cores via PIE/SIMD. The 16×INT8 MAC/cycle ceiling limits peak throughput.

### 6. ESP-DL model format instability
`.espdl` model format is not forward-compatible across major ESP-DL versions. Models quantized with
an older ESP-PPQ must be re-quantized when upgrading ESP-DL.

### 7. ONNX Runtime not deployable
ONNX is only a quantization pipeline input. ONNX Runtime cannot run directly on the P4.

### 8. TCM stack allocation bug
Allocating `TimerTask` stack in TCM memory crashes the P4 (ESP-IDF issue IDFGH-15337). Do not
allocate timer task stacks in TCM.

---

## Verdict for wildlife trap

The P4NRW32 is the most capable ESP32-class chip for camera-based edge AI, but for this project's
workload it sits **below the Luckfox Pico Zero**:

- **Slower inference** — 611 ms vs. ~300–500 ms on Luckfox (which has a proper RKNN NPU)
- **No on-chip wireless** — requires a companion chip, whereas Luckfox has AIC8800DC built in
- **Cheaper die** — ~$4.52 vs. ~$7 Luckfox module, and the P4 adds hardware ISP + H.264 that
  Luckfox lacks; compelling on a custom PCB pairing P4 (vision) + ESP32-C6 (wireless)
- **FreeRTOS only** — no Linux, harder to iterate on model deployment vs. Luckfox

Best fit: a future custom PCB design where bill-of-materials cost matters, a hardware ISP or H.264
encoder is needed, and a two-chip (P4 + C6) architecture is acceptable. Not a drop-in replacement
for the Luckfox module today.

The ESP-DL ecosystem is maturing rapidly (YOLOv26 added March 2026, new QAT tooling February 2026)
and is worth re-evaluating as a custom-PCB option in 6–12 months.
