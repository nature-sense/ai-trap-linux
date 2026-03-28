#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
#  convert-rknn.sh
#
#  Converts a yolo11n ONNX model to RKNN INT8 for the RV1106 NPU.
#  Runs rknn-toolkit2 inside Docker (x86_64) — works on Mac (Intel or Apple Silicon).
#
#  Usage:
#    ./scripts/convert-rknn.sh <model.onnx> <model.rknn> [calibration_dir]
#
#  Arguments:
#    model.onnx        yolo11n ONNX export (320×320, single-class)
#    model.rknn        output path for the .rknn file
#    calibration_dir   optional: directory of JPEG/PNG images for INT8 calibration
#                      (50–200 representative images from the trap camera)
#                      Omit for a fp16 build (slower on NPU, no images needed).
#
#  Examples:
#    # INT8 (recommended):
#    ./scripts/convert-rknn.sh firmware/models/yolo11n-320/model.onnx \
#                               firmware/models/yolo11n-320/model.rknn \
#                               /path/to/calibration_images
#
#    # fp16 (no calibration images):
#    ./scripts/convert-rknn.sh firmware/models/yolo11n-320/model.onnx \
#                               firmware/models/yolo11n-320/model.rknn
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

ONNX_FILE="${1:-}"
RKNN_FILE="${2:-}"
CALIB_DIR="${3:-}"

if [ -z "$ONNX_FILE" ] || [ -z "$RKNN_FILE" ]; then
    echo "Usage: $0 <model.onnx> <model.rknn> [calibration_dir]" >&2
    exit 1
fi

if [ ! -f "$ONNX_FILE" ]; then
    echo "ERROR: ONNX file not found: $ONNX_FILE" >&2
    exit 1
fi

# Resolve to absolute paths for Docker volume mounts
ONNX_ABS="$(cd "$(dirname "$ONNX_FILE")" && pwd)/$(basename "$ONNX_FILE")"
RKNN_ABS="$(cd "$(dirname "$RKNN_FILE")" && pwd)/$(basename "$RKNN_FILE")"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

DOCKER_ARGS=(
    --rm
    --platform linux/amd64
    -v "$REPO_ROOT:/workspace"
    -v "$(dirname "$ONNX_ABS"):/models"
)

CMD_ARGS=(
    python3 /workspace/scripts/convert_rknn.py
    "/models/$(basename "$ONNX_ABS")"
    "/models/$(basename "$RKNN_ABS")"
)

if [ -n "$CALIB_DIR" ]; then
    if [ ! -d "$CALIB_DIR" ]; then
        echo "ERROR: calibration directory not found: $CALIB_DIR" >&2
        exit 1
    fi
    CALIB_ABS="$(cd "$CALIB_DIR" && pwd)"
    DOCKER_ARGS+=(-v "$CALIB_ABS:/calibration:ro")
    CMD_ARGS+=(/calibration)
fi

echo "=== RKNN conversion ==="
echo "  ONNX : $ONNX_ABS"
echo "  RKNN : $RKNN_ABS"
[ -n "$CALIB_DIR" ] && echo "  calib: $CALIB_ABS (INT8)" || echo "  quant: none (fp16)"
echo ""

# Build or reuse the toolkit image
IMAGE="rknn-toolkit2:local"
if ! docker image inspect "$IMAGE" &>/dev/null; then
    echo "=== Building rknn-toolkit2 Docker image (first run, ~5 min) ==="
    docker build --platform linux/amd64 -t "$IMAGE" - <<'DOCKERFILE'
FROM python:3.10-slim
RUN apt-get update -qq && \
    apt-get install -y --no-install-recommends \
        libgomp1 libglib2.0-0 libsm6 libxrender1 libxext6 && \
    rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir \
    rknn-toolkit2 \
    onnx \
    onnxruntime \
    opencv-python-headless \
    numpy
DOCKERFILE
    echo "=== Image built ==="
fi

echo "=== Running conversion ==="
docker run "${DOCKER_ARGS[@]}" "$IMAGE" "${CMD_ARGS[@]}"

echo ""
echo "=== Done: $RKNN_ABS ==="
