#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  RknnInference — thin wrapper around the Rockchip RKNN NPU runtime.
//
//  Only compiled when -DUSE_RKNN is set (Luckfox RKNN build).
//  The ncnn dependency is still present for ncnn::Mat, which is used
//  throughout the capture and decoder pipeline.
//
//  Model preparation (x86 Ubuntu, via Docker on Mac):
//    pip install rknn-toolkit2
//    # Use RKNN Model Zoo YOLO conversion script to produce model.rknn from
//    # the ONNX export of yolo11n.  INT8 quantised, input 320×320.
//
//  Runtime dependency on device:
//    librknnrt.so   — from Luckfox SDK, installed to /opt/trap/ or sysroot
//    rknn_api.h     — header must be in sysroot/usr/include at cross-build time
//
//  Usage:
//    RknnInference net;
//    if (!net.init("model.rknn", 320, 320)) return 1;
//    ...
//    ncnn::Mat out;
//    if (!net.infer((float*)frame.modelInput.data, out)) return;
//    auto dets = decoder.decode(out, ...);
// ─────────────────────────────────────────────────────────────────────────────

#include "ncnn/mat.h"
#include <cstdint>
#include <string>
#include <vector>

class RknnInference {
public:
    RknnInference()  = default;
    ~RknnInference() { deinit(); }

    RknnInference(const RknnInference&)            = delete;
    RknnInference& operator=(const RknnInference&) = delete;

    // Load a .rknn model file.  inputW/inputH must match the model's input shape.
    // Returns true on success.
    bool init(const std::string& modelPath, int inputW, int inputH);

    // Run one inference pass.  inputCHW must point to inputH × inputW × 3
    // channel-planar float32 values (NCHW layout, matching ncnn::Mat).
    // On success, sets `out` to an ncnn::Mat wrapping the output buffer —
    // valid until the next call to infer() or deinit().
    // Returns false on failure.
    bool infer(const float* inputCHW, ncnn::Mat& out);

    void deinit();

private:
    uint64_t              m_ctx      = 0;   // rknn_context (typedef uint64_t)
    int                   m_inputW   = 0;
    int                   m_inputH   = 0;
    int                   m_outW     = 0;   // ncnn Mat w (number of anchors)
    int                   m_outH     = 0;   // ncnn Mat h (4 + numClasses)
    std::vector<uint8_t>  m_inputBuf;       // float32 CHW → uint8 HWC staging buffer
    std::vector<float>    m_outBuf;         // dequantised output, owned here
};
