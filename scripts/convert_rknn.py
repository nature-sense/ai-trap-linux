#!/usr/bin/env python3
"""
Convert a yolo11n ONNX model to RKNN INT8 for the RV1106 NPU.

Usage (inside Docker — see convert-rknn.sh):
    python3 convert_rknn.py <model.onnx> <model.rknn> [calibration_dir]

Arguments:
    model.onnx        Input ONNX model (yolo11n, 320x320, single-class)
    model.rknn        Output RKNN model path
    calibration_dir   Optional: directory of JPEG/PNG images for INT8 quantisation
                      (recommended: 50–200 representative images)
                      If omitted, the model is built without quantisation (fp16).

Input normalisation contract (must match runtime in rknn_infer.cpp):
    RKNN config: mean=[0,0,0]  std=[255,255,255]
    → RKNN applies ÷255 internally; runtime passes uint8 NHWC [0,255].
"""

import sys
import os

def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    onnx_path  = sys.argv[1]
    rknn_path  = sys.argv[2]
    calib_dir  = sys.argv[3] if len(sys.argv) > 3 else None

    if not os.path.isfile(onnx_path):
        print(f'ERROR: ONNX model not found: {onnx_path}', file=sys.stderr)
        sys.exit(1)

    from rknn.api import RKNN

    rknn = RKNN(verbose=False)

    # ── Configure ─────────────────────────────────────────────────────────────
    # mean/std must match rknn_infer.cpp: runtime sends uint8 NHWC [0,255],
    # RKNN divides by std=255 internally to produce the [0,1] float the model
    # was trained on.
    print('[rknn] configuring for rv1106...')
    ret = rknn.config(
        mean_values          = [[0, 0, 0]],
        std_values           = [[255, 255, 255]],
        target_platform      = 'rv1106',
        quantized_dtype      = 'asymmetric_quantized-8',
        quantized_algorithm  = 'normal',
        optimization_level   = 3,
        quant_img_RGB2BGR    = False,   # model expects RGB
    )
    assert ret == 0, f'rknn.config failed: {ret}'

    # ── Load ONNX ─────────────────────────────────────────────────────────────
    print(f'[rknn] loading ONNX: {onnx_path}')
    ret = rknn.load_onnx(model=onnx_path)
    assert ret == 0, f'rknn.load_onnx failed: {ret}'

    # ── Build ─────────────────────────────────────────────────────────────────
    if calib_dir:
        # Write dataset.txt: one image path per line
        image_exts = ('.jpg', '.jpeg', '.png', '.bmp')
        images = sorted(
            os.path.join(calib_dir, f)
            for f in os.listdir(calib_dir)
            if f.lower().endswith(image_exts)
        )
        if not images:
            print(f'WARNING: no images found in {calib_dir}, building without quantisation',
                  file=sys.stderr)
            calib_dir = None
        else:
            dataset_txt = '/tmp/rknn_dataset.txt'
            with open(dataset_txt, 'w') as f:
                f.write('\n'.join(images) + '\n')
            print(f'[rknn] INT8 quantisation using {len(images)} images from {calib_dir}')

    do_quant = calib_dir is not None
    if not do_quant:
        print('[rknn] building without quantisation (fp16 — slower on NPU)')

    ret = rknn.build(
        do_quantization = do_quant,
        dataset         = dataset_txt if do_quant else None,
    )
    assert ret == 0, f'rknn.build failed: {ret}'

    # ── Export ────────────────────────────────────────────────────────────────
    print(f'[rknn] exporting: {rknn_path}')
    ret = rknn.export_rknn(rknn_path)
    assert ret == 0, f'rknn.export_rknn failed: {ret}'

    print(f'[rknn] done → {rknn_path}')
    rknn.release()

if __name__ == '__main__':
    main()
