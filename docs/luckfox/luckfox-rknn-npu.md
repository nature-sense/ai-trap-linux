# Luckfox Pico Zero — RKNN NPU Inference

**Updated:** April 2026
**Scope:** Building and deploying the Luckfox firmware with RV1106 NPU inference

---

## Overview

The RV1106G3 SoC on the Luckfox Pico Zero includes a **0.5 TOPS NPU** accessible via
Rockchip's RKNN runtime (`librknnrt`).  Inference on the NPU is roughly **10× faster**
than the Cortex-A7 CPU: ~30–50 ms vs ~350 ms per frame for YOLO11n 320×320.

The Luckfox firmware uses the NPU exclusively.  ncnn is retained as a library because
`ncnn::Mat` is the shared tensor transport type used by the capture and decoder pipeline,
but `ncnn::Net` is never called at runtime.

```
V4L2 capture (NV12)
    ↓
NV12 → RGB → letterbox → float32 CHW [0,1]
    ↓
float32 CHW → uint8 HWC [0,255]   (rknn_infer.cpp)
    ↓
RV1106 NPU — RKNN INT8 model      (~30–50 ms)
    ↓
dequantised float32 [1,5,2100]    wrapped as ncnn::Mat
    ↓
YoloDecoder → ByteTracker → SQLite / crops / MJPEG
```

---

## Prerequisites

| Item | Required for |
|------|-------------|
| Docker Desktop | Model conversion + cross-compilation on Mac |
| ADB | Device deployment via `luckfox-install.sh` |
| `model.onnx` | RKNN conversion input (see Step 1) |
| Calibration images | INT8 quantisation — 50–200 trap camera frames (optional but recommended) |

The build and install scripts handle everything else automatically — toolchain download,
`librknnrt.so` fetch from the Luckfox SDK, cross-compilation — and cache all downloads
in `~/.cache/ai-trap/luckfox/`.

---

## Step 1 — Get or train a model

You need a YOLO11n (or compatible) model exported to ONNX at 320×320.

### Export from a trained .pt file

```python
from ultralytics import YOLO
model = YOLO('yolo11n-insect.pt')
model.export(format='onnx', imgsz=320, simplify=True, opset=12)
# → yolo11n-insect.onnx
```

Copy the result to `firmware/models/yolo11n-320/model.onnx`.

The default Ultralytics ONNX export does **not** bake NMS into the graph — this is
correct.  The firmware decoder handles NMS internally.

### Use the CI pipeline (automatic)

If `firmware/models/yolo11n-320/model.pt` is committed to the `ai-trap-models`
submodule, the **Convert model** GitHub Actions workflow generates `model.onnx`,
`model.ncnn.param/bin`, and `model.rknn` automatically on every push.

---

## Step 2 — Convert ONNX → RKNN

Conversion uses `rknn-toolkit2` running inside Docker.  Works on Intel and Apple
Silicon Macs.  The Docker image is built once (~10 min on Apple Silicon under QEMU)
and then cached.

### INT8 (recommended — needs calibration images)

Calibration images should be representative of what the trap camera will see in the
field.  Crops saved by the trap (`/opt/trap/crops/*.jpg`) work well.  50–200 images
is sufficient.

```bash
bash firmware/models/scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn \
    /path/to/calibration_images
```

### fp16 (no calibration images needed)

```bash
bash firmware/models/scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn
```

fp16 models are still significantly faster than CPU inference but slower than INT8
on the NPU.

### What the conversion does

1. Builds (once) or reuses the `rknn-toolkit2:local` Docker image
2. Runs `convert_rknn.py` with `target_platform='rv1106'`, `mean=[0,0,0]`, `std=[255,255,255]`
3. Writes `model.rknn` alongside the ONNX file

> **Normalisation contract:** The model is converted with `std=[255,255,255]`, so
> RKNN applies `÷255` internally.  The runtime (`rknn_infer.cpp`) sends `uint8 NHWC
> [0,255]` — pixel preprocessing is otherwise unchanged from a standard YOLO pipeline.

---

## Step 3 — Build

```bash
bash scripts/build-luckfox-mac.sh
```

First run: ~25 min (toolchain + SQLite3 + NCNN + RKNN library fetch — all cached)
Subsequent runs: ~2 min

What the script does:
1. Launches an `ubuntu:22.04` Docker container (`linux/amd64`)
2. Downloads the Luckfox cross-compiler toolchain (cached)
3. Cross-compiles SQLite3 and NCNN (cached)
4. Fetches `librknnrt.so` and `rknn_api.h` from the Luckfox SDK (cached)
5. Builds with meson + ninja, strips the binary

Output:
```
build-luckfox/yolo_v4l2
~/.cache/ai-trap/luckfox/sysroot/usr/lib/librknnrt.so
```

---

## Step 4 — Deploy

```bash
bash scripts/luckfox-install.sh
```

What gets installed on the device:

| Path | Content |
|------|---------|
| `/opt/trap/yolo_v4l2` | Application binary |
| `/opt/trap/model.rknn` | RKNN INT8/fp16 model |
| `/opt/trap/librknnrt.so` | Rockchip NPU runtime |
| `/opt/trap/trap_config.toml` | Configuration |
| `/etc/init.d/S99trap` | Init script |

The generated `S99trap` starts the binary with:

```sh
export LD_LIBRARY_PATH=/opt/trap
/opt/trap/yolo_v4l2 \
    /opt/trap/model.rknn \
    /opt/trap/detections.db \
    /opt/trap/crops \
    /dev/video0
```

> `librknnrt.so` lives in `/opt/trap/` (writable on Buildroot) rather than
> `/usr/lib/` (read-only).  `LD_LIBRARY_PATH` in `S99trap` ensures the dynamic
> linker finds it without needing `ldconfig`.

---

## Step 5 — Verify

```bash
ssh root@<board-ip>
/etc/init.d/S99trap restart
tail -f /var/log/ai-trap.log
```

The binary prints RKNN model details on startup:

```
══════════════════════════════════════════════════════════
  YOLO11n  Luckfox Pico Zero  IMX415 V4L2 + RKNN NPU + ByteTracker
══════════════════════════════════════════════════════════
  model  : /opt/trap/model.rknn
  db     : /opt/trap/detections.db
  crops  : /opt/trap/crops
  device : /dev/video0

[rknn] model inputs=1  outputs=1
[rknn] output tensor: n_dims=3  shape=[1,5,2100]  → ncnn::Mat(2100, 5)
RKNN model loaded: /opt/trap/model.rknn
```

---

## Adding INT8 calibration images to CI

Committing images to `firmware/models/calibration/` enables INT8 quantisation in the
GitHub Actions conversion workflow:

```bash
# Pull crops from a running trap
scp root@<board-ip>:/opt/trap/crops/*.jpg firmware/models/calibration/

cd firmware/models
git add calibration/
git commit -m "feat: add INT8 calibration images (N frames)"
git push
```

The **Convert model** workflow detects the images and runs INT8 conversion
automatically.  The resulting `model.rknn` is committed back to the submodule.

---

## How the runtime works

| Stage | Detail |
|-------|--------|
| **V4L2 capture** | NV12 from `/dev/video0` via `ioctl VIDIOC_DQBUF` |
| **Preprocessing** | NV12 → RGB float32 CHW `[0,1]` → letterbox to 320×320 |
| **Input staging** | `float32 CHW [0,1]` → `uint8 HWC [0,255]` in `rknn_infer.cpp` |
| **RKNN normalisation** | RKNN applies `÷255` internally (`std=[255,255,255]` set at conversion) |
| **NPU inference** | INT8 quantised forward pass on RV1106 0.5 TOPS NPU |
| **Output** | Dequantised `float32`, shape `[1, 5, 2100]`, wrapped as `ncnn::Mat(2100, 5)` |
| **Postprocessing** | `YoloDecoder` → NMS → `ByteTracker` → SQLite / crops / MJPEG |

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| `rknn_init failed: -1` | Model file not found or corrupt — verify path, re-run conversion |
| `rknn_init failed: -9` | Wrong target platform — model must use `target_platform='rv1106'` |
| `rknn_inputs_set failed` | Input size mismatch — `modelWidth`/`modelHeight` in `main_v4l2.cpp` must match ONNX `imgsz` |
| `librknnrt.so: not found` | Library not in `LD_LIBRARY_PATH` — check `S99trap` has `export LD_LIBRARY_PATH=/opt/trap` |
| Detections all zero or implausible | Normalisation mismatch — ensure conversion used `std=[255,255,255]` |
| `rknn_query OUTPUT_ATTR failed` | Unexpected output shape — re-convert with latest `convert_rknn.py` |
| librknnrt.so missing from build cache | Wipe and rebuild: `rm -rf ~/.cache/ai-trap/luckfox/sysroot && bash scripts/build-luckfox-mac.sh` |
