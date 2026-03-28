# Luckfox Pico Zero — RKNN NPU Inference

## Overview

The RV1106G3 SoC on the Luckfox Pico Zero includes a 0.5 TOPS NPU accessible via
Rockchip's RKNN runtime (`librknnrt`).  Running inference on the NPU instead of the
Cortex-A7 CPU yields roughly a **10× speedup** for yolo11n 320×320 (20–50 ms vs
300–500 ms per frame).

The default firmware build uses **ncnn on CPU**.  The RKNN build is an opt-in
variant controlled by `-Duse_rknn=true` at configure time.  All other pipeline
components (capture, decode, track, persist, stream) are identical between the two
builds.

---

## Prerequisites

| Item | Where to get it |
|------|----------------|
| Docker Desktop | [docker.com](https://www.docker.com/products/docker-desktop/) — required for model conversion on Mac |
| Luckfox SDK | `git clone https://github.com/LuckfoxTECH/luckfox-pico.git` |
| yolo11n ONNX | Export from Ultralytics (see below) |
| Calibration images | 50–200 JPEG/PNG frames from the trap camera (optional but recommended) |

---

## 1. Export the ONNX model

If you have a trained Ultralytics yolo11n `.pt` file:

```python
from ultralytics import YOLO
model = YOLO('yolo11n-insect.pt')
model.export(format='onnx', imgsz=320, simplify=True, opset=12)
# produces yolo11n-insect.onnx
```

The default Ultralytics ONNX export does **not** bake NMS into the graph — this is
what we want.  The firmware decoder handles NMS internally.

---

## 2. Convert to RKNN (Mac)

Conversion uses rknn-toolkit2 running inside Docker (`--platform linux/amd64`).
Works on both Intel and Apple Silicon Macs.

### First run — build the Docker image

The image only needs to be built once.  On Apple Silicon it runs under QEMU
emulation; expect ~10 minutes.  Subsequent builds are instant (cached).

```bash
docker build \
  --platform linux/amd64 \
  -t rknn-toolkit2:local \
  -f scripts/Dockerfile.rknn-toolkit2 .
```

### INT8 conversion (recommended)

INT8 quantisation requires a calibration dataset — a text file listing image paths
that are representative of what the camera will see in the field.  Crops saved by
the trap (in the `crops/` directory) work well.

```bash
./scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn \
    /path/to/calibration_images
```

### fp16 conversion (no calibration images required)

```bash
./scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn
```

fp16 models are slower on the NPU than INT8 but still significantly faster than
CPU inference.

### What the script does

1. Checks for the `rknn-toolkit2:local` Docker image; builds it if absent.
2. Mounts the model directory and (optionally) calibration directory into the container.
3. Runs `scripts/convert_rknn.py` with `target_platform=rv1106`,
   `mean=[0,0,0]`, `std=[255,255,255]`.
4. Writes `model.rknn` alongside the ONNX file.

---

## 3. Install librknnrt on the build host

`librknnrt.so` and its header are **not** included in the GitHub repository — they
ship with the Luckfox SDK.

```bash
SYSROOT=/opt/luckfox-sysroot
SDK=luckfox-pico   # path to your SDK clone

# Header (needed at cross-compile time)
cp $SDK/sysdrv/source/npu/rknn/librknnrt/include/rknn_api.h \
   $SYSROOT/usr/include/

# Shared library (needed at link time and on the board)
cp $SDK/sysdrv/source/npu/rknn/librknnrt/lib/librknnrt.so \
   $SYSROOT/usr/lib/
```

---

## 4. Build the RKNN firmware

Add `-Duse_rknn=true` to the meson configure step:

```bash
meson setup build-luckfox-rknn \
  --cross-file cross/luckfox-rv1106.ini \
  -Dtarget=v4l2 \
  -Duse_rknn=true \
  -Dluckfox_sysroot=/opt/luckfox-sysroot
ninja -C build-luckfox-rknn
```

This produces `build-luckfox-rknn/yolo_v4l2` compiled with `-DUSE_RKNN`.
The ncnn inference path is compiled out entirely.

Strip before deploying:

```bash
arm-rockchip830-linux-uclibcgnueabihf-strip build-luckfox-rknn/yolo_v4l2
```

---

## 5. Deploy to the board

`librknnrt.so` must be present on the board at runtime.

```bash
# Copy runtime library (first time only)
scp $SYSROOT/usr/lib/librknnrt.so root@<board-ip>:/opt/trap/

# On the board: make it findable
echo '/opt/trap' >> /etc/ld.so.conf
ldconfig
```

Then deploy the binary and model:

```bash
scp build-luckfox-rknn/yolo_v4l2               root@<board-ip>:/opt/trap/
scp firmware/models/yolo11n-320/model.rknn      root@<board-ip>:/opt/trap/
```

Update the init script (`/etc/init.d/S99trap`) to pass the `.rknn` file instead
of the two ncnn files.  The RKNN build takes a single model argument:

```
# ncnn build:   ./yolo_v4l2 model.param model.bin  [db] [crops] [device]
# RKNN build:   ./yolo_v4l2 model.rknn              [db] [crops] [device]
```

---

## 6. Verify on the board

```bash
ssh root@<board-ip>
/etc/init.d/S99trap restart
tail -f /var/log/ai-trap.log
```

On startup the binary prints the RKNN model details:

```
══════════════════════════════════════════════════════════
  YOLO11n  Luckfox Pico Zero  IMX415 V4L2 + RKNN NPU + ByteTracker
══════════════════════════════════════════════════════════
  model  : /opt/trap/model.rknn
  ...

[rknn] model inputs=1  outputs=1
[rknn] output tensor: n_dims=3  shape=[1,5,2100]  → ncnn::Mat(2100, 5)
RKNN model loaded: /opt/trap/model.rknn
```

---

## How the runtime works

| Stage | Detail |
|-------|--------|
| **Preprocessing** | V4L2 capture → NV12 → RGB → letterbox → float32 CHW `[0,1]` (unchanged from ncnn path) |
| **Input conversion** | `float32 CHW [0,1]` → `uint8 HWC [0,255]` in `rknn_infer.cpp` |
| **RKNN normalisation** | RKNN applies `÷255` internally (configured via `std=[255,255,255]` at conversion time) |
| **NPU inference** | INT8 quantised forward pass on RV1106 NPU |
| **Output** | Dequantised `float32`, shape `(5, 2100)`, wrapped in `ncnn::Mat` |
| **Postprocessing** | `YoloDecoder` → `ByteTracker` → SQLite / crops / MJPEG (identical to ncnn path) |

---

## Troubleshooting

| Symptom | Check |
|---------|-------|
| `rknn_init failed: -1` | Model file not found or corrupt — verify path and re-run conversion |
| `rknn_init failed: -9` | Wrong target platform — model must be converted with `target_platform='rv1106'` |
| `rknn_inputs_set failed` | Input size mismatch — recheck `modelWidth`/`modelHeight` in `main_v4l2.cpp` match the ONNX export `imgsz` |
| `librknnrt.so: not found` | Library not in search path — run `ldconfig` after installing to `/opt/trap/` |
| Detections all zero / implausible | Normalisation mismatch — ensure model was converted with `std=[255,255,255]` |
| Unexpectedly slow | Check with `top` — if inference is on CPU, the RKNN binary may not have loaded correctly |
