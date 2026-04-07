# ai-trap-linux — Software Architecture (Luckfox Target)

## Hardware target

| Item | Detail |
|------|--------|
| Board | Luckfox Pico Zero (RV1106G3, Cortex-A7 @ 1.2 GHz) |
| Kernel | Linux 5.10.160 (Luckfox BSP) |
| Camera | IMX415-98 IR-CUT, V4L2 NV12, 1920×1080 @ 30 fps |
| NPU | Rockchip RV1106 |

---

## Components and versions

| Component | Version | Role |
|-----------|---------|------|
| **librknnmrt.so** | v2.3.2 | RKNN NPU mini-runtime — zero-copy DMA inference on RV1106 |
| **rknpu.ko** | 0.9.8 (Rockchip develop-5.10) | NPU kernel driver, loaded via `/oem/usr/ko/insmod_ko.sh` |
| **SQLite3** | 3.51.3 | Detection/track persistence database |
| **YOLO11n** | 320×320, stripped-DFL format | Insect detection model, converted to `.rknn` via rknn-toolkit2 v2.3.2 |
| **rknn-toolkit2** | v2.3.2 | ONNX → RKNN model conversion (Docker, x86-64 host) |
| **Luckfox BSP toolchain** | arm-rockchip830-linux-uclibcgnueabihf | Cross-compiler for Cortex-A7 / uClibc |

---

## Source layout

```
firmware/
  main_v4l2.cpp             — entry point, wires all components together
  src/
    common/
      float_mat.h           — FloatMat: lightweight CHW float tensor
      imgproc.h             — nv12_to_rgb_u8(), bilinear_resize_rgb_u8()
      stb_image_write.h/cpp — JPEG encoding
      wifi_manager.h/cpp    — WiFi AP/station management (hostapd backend)
      ble_gatt_server.h/cpp — BLE GATT for WiFi credential provisioning
      epaper_display.h/cpp  — e-Paper status display
    io/
      v4l2_capture.h/cpp    — V4L2 capture, NV12 de-stride, preprocess → FloatMat
      rknn_infer.h/cpp      — RKNN NPU inference wrapper (zero-copy DMA API)
    pipeline/
      decoder.h/cpp         — YOLO output decoder (DFL / AnchorGrid / EndToEnd formats)
      tracker.h/cpp         — ByteTracker multi-object tracker
      persistence.h/cpp     — SQLite3 writer
      crop_saver.h/cpp      — async best-confidence JPEG crop saver with EXIF
      exif_writer.h/cpp     — EXIF metadata injection (trackId, GPS, timestamp)
      sync_manager.h/cpp    — crop/detection sync state for REST API
    server/
      mjpeg_streamer.h/cpp  — MJPEG stream server (TCP, port 9000)
      sse_server.h/cpp      — Server-Sent Events (port 8081)
      http_server.h/cpp     — REST API (port 8080)
```

---

## Threading model

| Thread | Owner | Responsibility |
|--------|-------|----------------|
| **capture** | `V4L2Capture` | `select()` + `VIDIOC_DQBUF` loop; de-strides NV12; re-queues buffer immediately; pushes `RawFrame` to queue |
| **dispatch** | `V4L2Capture` | Pops `RawFrame`; calls `preprocess()` → `FloatMat`; fires `FrameCallback` |
| RKNN inference | runs on dispatch thread | `rknn_init` deferred to first call (mini-runtime requires same-thread init+run); `rknn_run()` per frame |
| **crop worker** | `CropSaver` | Async JPEG encode + EXIF inject; one worker thread, bounded queue |
| **MJPEG accept** | `MjpegStreamer` | `accept()` loop; spawns one detached thread per client |
| **MJPEG client** | `MjpegStreamer` | One thread per connected client; woken by `m_frameCv` condition variable |
| **SSE** | `SseServer` | Accept + per-client thread |
| **HTTP** | `HttpServer` | Accept + per-request thread |
| **BLE GATT** | `BleGattServer` | HCI raw socket event loop |

The inference pipeline runs entirely on the dispatch thread:
`preprocess → rknn_run → decode → track → DB write → crop submit → stream push`.

This is intentional — the RV1106 NPU has a single inference core and the RKNN mini-runtime is not thread-safe.

---

## Inference pipeline (per frame)

```
V4L2 DMA buffer (NV12 1920×1080)
  ↓ de-stride (captureLoop)
  ↓ nv12_to_rgb_u8()              BT.601 limited-range, packed HWC uint8
  ↓ bilinear_resize_rgb_u8()      1920×1080 → 320×320 letterboxed
  ↓ FloatMat CHW float [0,1]      3 × 320 × 320 = 307 200 floats
  ↓ rknn_run()                    NPU ~25 ms on RV1106 @ 1.2 GHz
  ↓ INT8 output dequantised       FloatMat(w=2100, h=65) stripped-DFL
  ↓ YoloDecoder::decodeDFL()      DFL softmax + anchor grid → detections
  ↓ ByteTracker::update()         IoU + Kalman tracking
  ↓ SQLite + CropSaver + MJPEG
```

**MJPEG white-balance correction**: `R×1.80, G×1.00, B×1.55` applied in
`MjpegStreamer::encodeFrame()` after NV12→RGB, before JPEG encode. Compensates
for ISP running without rkaiq AWB/CCM on RV1106. Applied to the stream only —
model input is raw, unmodified pixel values.

---

## RKNN zero-copy DMA API

The RV1106 mini-runtime (`librknnmrt.so`) does not support
`rknn_inputs_set`/`rknn_outputs_get` (returns -5 "context config invalid").
The correct path:

```
rknn_create_mem(ctx, size)        allocate DMA-coherent buffer once at init
rknn_set_io_mem(ctx, mem, attr)   bind buffer to input/output tensor
write virt_addr                   per frame: float CHW [0,1] → uint8 HWC [0,255]
rknn_run(ctx)
read virt_addr                    INT8 output; dequantise: float = (int8 - zp) * scale
```

Input and output buffers are allocated once in `lazyInitCtx()` and reused every
frame. Context creation (`rknn_init`) is deferred to the first `infer()` call to
satisfy the mini-runtime's same-thread requirement.

### Self-healing watchdog

On `rknn_run` failure, `RknnInference` tears down the context and DMA buffers,
sets `m_readyToInit = true`, and returns `false` for that frame. The next
`infer()` call triggers `lazyInitCtx()` which rebuilds everything. The NPU
driver performs a soft reset after each timeout, so recovery is typically
immediate with no reboot required.

Counters `m_consecutiveFailures` and `m_totalRecoveries` are logged to stderr.

### NPU polling mode

`bypass_irq_handler=1` is set in `/oem/usr/S99trap` before launching
`yolo_v4l2`. This puts the NPU driver in polling mode, preventing ISP DMA
traffic from starving the NPU completion interrupt (IRQ 37).

> **Note**: killing `yolo_v4l2` with `SIGKILL` during `rknn_run` corrupts
> driver state. Recovery requires `rmmod rknpu && insmod /oem/usr/ko/rknpu.ko`.
> Use `SIGTERM` for clean shutdown.

---

## YOLO model format — stripped-DFL

The model is exported to RKNN with the DFL regression head stripped and
pre-processed on the NPU. Output tensor layout:

```
dims=2,  h = 4 * reg_max + numClasses  (= 65 for single-class, reg_max=16)
         w = numAnchors                 (= 2100 for 320×320 input)

rows 0 .. 63  : raw DFL regression logits (4 directions × 16 bins)
row  64       : sigmoid class score (pre-applied by model)
```

The C++ decoder (`YoloDecoder::decodeDFL`) builds the anchor grid for strides
8/16/32, applies softmax-weighted-sum DFL, reconstructs bounding boxes, and
runs NMS.

---

## Build system

- **Build tool**: Meson + Ninja
- **Cross-compilation host**: Ubuntu 22.04 container (`linux/amd64`) via Docker on macOS
- **Script**: `scripts/build-luckfox-mac.sh`

### Dependencies fetched/built by the script

| Dependency | Source | Notes |
|-----------|--------|-------|
| Luckfox toolchain | `LuckfoxTECH/luckfox-pico` (sparse clone) | Cached in `~/.cache/ai-trap/luckfox/toolchain/` |
| BlueZ headers | Host apt `libbluetooth-dev` | Copied into sysroot; no `.so` linked |
| SQLite3 3.51.3 | sqlite.org tarball | Cross-compiled static `.a`; cached in sysroot |
| librknnmrt.so v2.3.2 | `airockchip/rknn-toolkit2` GitHub release | Armhf-uclibc variant; cached in sysroot |

### rknpu.ko

Built separately by `scripts/build-rknpu-ko.sh`:

- Sparse-clones Luckfox BSP kernel source (5.10.160) into `~/.cache/ai-trap/luckfox/kernel-src/`
- Applies two BSP compatibility patches:
  - Copies `include/linux/version_compat_defs.h` from Rockchip develop-5.10 (absent from Luckfox BSP)
  - Fixes `MONITOR_TPYE_` typo in `include/soc/rockchip/rockchip_system_monitor.h`
- Runs `make modules_prepare` (stamped; skipped on rebuild)
- Fetches all 22 rknpu 0.9.8 source files from Rockchip develop-5.10
- Builds `rknpu.ko` with `make M=drivers/rknpu modules`
- Verifies vermagic: `5.10.160 mod_unload ARMv7 thumb2 p2v8`

---

## Deployment

```bash
# Full build and deploy
bash scripts/build-luckfox-mac.sh
bash scripts/luckfox-install.sh

# Hot-swap rknpu.ko without reboot
bash scripts/build-rknpu-ko.sh
bash scripts/luckfox-install.sh --rknpu-only

# Model conversion (ONNX → RKNN)
bash firmware/models/scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn \
    firmware/models/calibration
```

---

## Network services

| Service | Port | Protocol |
|---------|------|----------|
| REST API | 8080 | HTTP |
| Server-Sent Events | 8081 | HTTP/SSE |
| MJPEG stream | 9000 | HTTP multipart |

View stream: `http://<device-ip>:9000/stream`
