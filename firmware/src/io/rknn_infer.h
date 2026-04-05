#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  RknnInference — thin wrapper around the Rockchip RKNN NPU runtime.
//
//  Only compiled when -DUSE_RKNN is set (Luckfox RKNN build).
//  The ncnn dependency is still present for ncnn::Mat, which is used
//  throughout the capture and decoder pipeline.
//
//  *** RV1106 / librknnmrt ZERO-COPY API ***
//
//  The mini runtime (librknnmrt.so) for RV1106/RV1103 does NOT support
//  rknn_inputs_set / rknn_outputs_get — those functions always return -5
//  "context config invalid".  The correct path is the zero-copy DMA API:
//
//    rknn_create_mem(ctx, size)         — allocate DMA-coherent memory
//    rknn_set_io_mem(ctx, mem, attr)    — bind memory to input/output tensor
//    write to mem->virt_addr            — copy frame data in
//    rknn_run(ctx)                      — execute inference
//    read from mem->virt_addr           — read INT8 output, dequantize manually
//
//  This is the pattern used in every official RV1106 demo (rknn_mobilenet_demo,
//  rknn_yolov5_demo) in the airockchip/rknn-toolkit2 GitHub repo.
//
//  Threading note:
//    The RKNN mini-runtime (librknnmrt) requires that rknn_init() and all
//    subsequent API calls happen in the same thread.  Context creation is
//    deferred to the first infer() call, which runs in the V4L2 dispatch thread.
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

// Forward-declare rknn types to avoid pulling rknn_api.h into every TU.
struct _rknn_tensor_memory;
typedef struct _rknn_tensor_memory rknn_tensor_mem;

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
    // Called on the first infer() to create the RKNN context in the
    // inference thread (mini-runtime requires same-thread init + run).
    bool lazyInitCtx();

    std::string           m_modelPath;           // path stored for logging
    std::vector<uint8_t>  m_modelBytes;          // raw model bytes loaded at init()
    bool                  m_readyToInit = false; // set by init(), cleared by lazyInitCtx()

    uint64_t              m_ctx         = 0;     // rknn_context (uint32_t on RV1106)

    // Zero-copy DMA buffers, allocated once in lazyInitCtx().
    rknn_tensor_mem*      m_inputMem    = nullptr;  // DMA buffer for model input
    rknn_tensor_mem*      m_outputMem   = nullptr;  // DMA buffer for model output (INT8)

    int                   m_inputW      = 0;
    int                   m_inputH      = 0;
    int                   m_inStride    = 0;     // w_stride from INPUT_ATTR (may > inputW)

    int                   m_outW        = 0;     // ncnn Mat w (number of anchors / columns)
    int                   m_outH        = 0;     // ncnn Mat h (4 + numClasses / rows)
    uint32_t              m_outElems    = 0;     // total output elements
    float                 m_outScale    = 1.f;   // affine dequant scale
    int32_t               m_outZp       = 0;     // affine dequant zero-point

    std::vector<float>    m_outBuf;              // dequantised float32 output
};
