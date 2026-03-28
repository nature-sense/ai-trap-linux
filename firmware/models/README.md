# models/

Model files for the ai-trap firmware.

## Source and generated files

| File | Type | Description |
|------|------|-------------|
| `yolo11n-320/model.pt` | **source** | Ultralytics yolo11n weights — commit this to trigger conversion |
| `yolo11n-320/model.ncnn.param` | generated | ncnn architecture — used by the default CPU build |
| `yolo11n-320/model.ncnn.bin` | generated | ncnn weights |
| `yolo11n-320/model.onnx` | generated | intermediate — also used for local Mac RKNN conversion |
| `yolo11n-320/model.rknn` | generated | RKNN INT8/fp16 — used by the `-Duse_rknn=true` NPU build |

All generated files are committed to the repository so that firmware builds and
deployments never require a local conversion step.

## Automatic conversion (CI)

Pushing `model.pt` to `main` triggers the **Convert model** GitHub Actions workflow
(`.github/workflows/convert-model.yml`).  The workflow:

1. Exports `model.ncnn.param` + `model.ncnn.bin` via Ultralytics ncnn export
2. Exports `model.onnx` via Ultralytics ONNX export
3. Converts `model.onnx` → `model.rknn` via rknn-toolkit2
4. Commits all outputs back to the repository with `[skip ci]`

No Docker or local tooling is needed — the runner is native x86_64 Linux.

### INT8 vs fp16 RKNN

If `firmware/models/calibration/` contains JPEG or PNG images, the RKNN model is
built with INT8 quantisation (better NPU throughput).  Without calibration images
it falls back to fp16.

To add calibration images:

```bash
mkdir -p firmware/models/calibration
# Copy 50–200 representative JPEG frames from the trap camera
cp /path/to/trap/crops/*.jpg firmware/models/calibration/
git add firmware/models/calibration/
git commit -m "feat: add INT8 calibration images"
git push
```

## Manual conversion on Mac

If you need to run the conversion locally (e.g. to test a new model before
committing, or to produce an INT8 build with a large local calibration set):

```bash
# INT8
./scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn \
    /path/to/calibration_images

# fp16 (no images needed)
./scripts/convert-rknn.sh \
    firmware/models/yolo11n-320/model.onnx \
    firmware/models/yolo11n-320/model.rknn
```

See `docs/luckfox-rknn-npu.md` for the full RKNN workflow.
