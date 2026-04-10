#include "rknn_infer.h"

#include <rknn_api.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  init  — loads the model file and defers rknn_init to the inference thread.
// ─────────────────────────────────────────────────────────────────────────────

bool RknnInference::init(const std::string& modelPath, int inputW, int inputH)
{
    m_inputW    = inputW;
    m_inputH    = inputH;
    m_modelPath = modelPath;

    std::ifstream f(modelPath, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        fprintf(stderr, "[rknn] cannot read model: %s\n", modelPath.c_str());
        return false;
    }
    auto size = f.tellg();
    f.seekg(0);
    m_modelBytes.resize(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(m_modelBytes.data()), size);
    if (!f) {
        fprintf(stderr, "[rknn] read error: %s\n", modelPath.c_str());
        return false;
    }

    fprintf(stderr, "[rknn] model loaded: %s (%lld bytes) — NPU context deferred\n",
            modelPath.c_str(), (long long)size);
    m_readyToInit = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  lazyInitCtx  — called on first infer() in the inference thread.
//
//  Uses the ZERO-COPY DMA API required by librknnmrt on RV1106/RV1103.
//  rknn_inputs_set / rknn_outputs_get are NOT supported by the mini runtime;
//  they always return -5 "context config invalid".
//  Correct pattern: rknn_create_mem + rknn_set_io_mem (allocated once here),
//  then write/read virt_addr per frame.
// ─────────────────────────────────────────────────────────────────────────────

bool RknnInference::lazyInitCtx()
{
    if (m_totalRecoveries > 0)
        fprintf(stderr, "[rknn] reinitialising NPU context (recovery #%d)...\n",
                m_totalRecoveries);

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx,
                        static_cast<void*>(m_modelBytes.data()),
                        static_cast<uint32_t>(m_modelBytes.size()),
                        0, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_init failed: %d\n", ret);
        return false;
    }
    m_ctx         = static_cast<uint64_t>(ctx);
    m_readyToInit = false;

    // Log runtime / driver version
    rknn_sdk_version sdk{};
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &sdk, sizeof(sdk)) == 0)
        fprintf(stderr, "[rknn] SDK %s  driver %s\n", sdk.api_version, sdk.drv_version);

    // Query tensor counts
    rknn_input_output_num io_num{};
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        fprintf(stderr, "[rknn] IN_OUT_NUM query failed: %d\n", ret);
        rknn_destroy(ctx); m_ctx = 0; return false;
    }
    fprintf(stderr, "[rknn] model: %u input(s), %u output(s)\n",
            io_num.n_input, io_num.n_output);

    // ── Input tensor ─────────────────────────────────────────────────────────
    rknn_tensor_attr in_attr{};
    in_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr));
    if (ret < 0) {
        fprintf(stderr, "[rknn] INPUT_ATTR query failed: %d\n", ret);
        rknn_destroy(ctx); m_ctx = 0; return false;
    }
    fprintf(stderr, "[rknn] input[0]: type=%d fmt=%d dims=[",
            (int)in_attr.type, (int)in_attr.fmt);
    for (uint32_t i = 0; i < in_attr.n_dims; ++i)
        fprintf(stderr, "%u%s", in_attr.dims[i], i + 1 < in_attr.n_dims ? "," : "");
    fprintf(stderr, "] size=%u size_with_stride=%u w_stride=%u\n",
            in_attr.size, in_attr.size_with_stride, in_attr.w_stride);

    // Override type to UINT8 NHWC — the model was converted with mean=[0,0,0]
    // std=[255,255,255] so the NPU expects uint8 values in [0,255].
    in_attr.type = RKNN_TENSOR_UINT8;
    in_attr.fmt  = RKNN_TENSOR_NHWC;

    m_inStride = (in_attr.w_stride > 0) ? (int)in_attr.w_stride : m_inputW;

    // Allocate DMA input buffer and bind it to the input tensor.
    uint32_t in_mem_sz = (in_attr.size_with_stride > 0)
                         ? in_attr.size_with_stride
                         : static_cast<uint32_t>(m_inputH * m_inStride * 3);
    m_inputMem = rknn_create_mem(ctx, in_mem_sz);
    if (!m_inputMem) {
        fprintf(stderr, "[rknn] rknn_create_mem (input) failed\n");
        rknn_destroy(ctx); m_ctx = 0; return false;
    }
    ret = rknn_set_io_mem(ctx, m_inputMem, &in_attr);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_set_io_mem (input) failed: %d\n", ret);
        rknn_destroy_mem(ctx, m_inputMem); m_inputMem = nullptr;
        rknn_destroy(ctx); m_ctx = 0; return false;
    }
    fprintf(stderr, "[rknn] input DMA mem: virt=%p size=%u\n",
            m_inputMem->virt_addr, in_mem_sz);

    // ── Output tensor ────────────────────────────────────────────────────────
    // Use NATIVE_NHWC_OUTPUT_ATTR — the correct query for RV1106 mini runtime.
    rknn_tensor_attr out_attr{};
    out_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
    if (ret < 0) {
        // Fallback to standard OUTPUT_ATTR if native query unsupported
        fprintf(stderr, "[rknn] NATIVE_NHWC_OUTPUT_ATTR failed (%d), trying OUTPUT_ATTR\n", ret);
        out_attr.index = 0;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
        if (ret < 0) {
            fprintf(stderr, "[rknn] OUTPUT_ATTR query failed: %d\n", ret);
            rknn_destroy_mem(ctx, m_inputMem); m_inputMem = nullptr;
            rknn_destroy(ctx); m_ctx = 0; return false;
        }
    }
    fprintf(stderr, "[rknn] output[0]: type=%d fmt=%d dims=[",
            (int)out_attr.type, (int)out_attr.fmt);
    for (uint32_t i = 0; i < out_attr.n_dims; ++i)
        fprintf(stderr, "%u%s", out_attr.dims[i], i + 1 < out_attr.n_dims ? "," : "");
    fprintf(stderr, "] size=%u size_with_stride=%u scale=%.6f zp=%d\n",
            out_attr.size, out_attr.size_with_stride, out_attr.scale, (int)out_attr.zp);

    // Map tensor dims to (h, w) for FloatMat(w, h).
    // Expected shapes after RKNN conversion of yolo11n:
    //   3D: [1, 5, 2100]  →  h=5,  w=2100
    //   2D: [5, 2100]     →  h=5,  w=2100
    if (out_attr.n_dims == 3) {
        m_outH = static_cast<int>(out_attr.dims[1]);
        m_outW = static_cast<int>(out_attr.dims[2]);
    } else if (out_attr.n_dims == 2) {
        m_outH = static_cast<int>(out_attr.dims[0]);
        m_outW = static_cast<int>(out_attr.dims[1]);
    } else {
        fprintf(stderr, "[rknn] unexpected output dims=%u\n", out_attr.n_dims);
        rknn_destroy_mem(ctx, m_inputMem); m_inputMem = nullptr;
        rknn_destroy(ctx); m_ctx = 0; return false;
    }

    m_outElems = static_cast<uint32_t>(m_outH) * m_outW;
    m_outScale = out_attr.scale;
    m_outZp    = out_attr.zp;
    m_outBuf.resize(m_outElems);

    // Allocate DMA output buffer and bind it.
    uint32_t out_mem_sz = (out_attr.size_with_stride > 0)
                          ? out_attr.size_with_stride
                          : static_cast<uint32_t>(m_outElems);  // INT8 = 1 byte/elem
    m_outputMem = rknn_create_mem(ctx, out_mem_sz);
    if (!m_outputMem) {
        fprintf(stderr, "[rknn] rknn_create_mem (output) failed\n");
        rknn_destroy_mem(ctx, m_inputMem); m_inputMem = nullptr;
        rknn_destroy(ctx); m_ctx = 0; return false;
    }
    ret = rknn_set_io_mem(ctx, m_outputMem, &out_attr);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_set_io_mem (output) failed: %d\n", ret);
        rknn_destroy_mem(ctx, m_outputMem); m_outputMem = nullptr;
        rknn_destroy_mem(ctx, m_inputMem);  m_inputMem  = nullptr;
        rknn_destroy(ctx); m_ctx = 0; return false;
    }
    fprintf(stderr, "[rknn] output DMA mem: virt=%p size=%u\n",
            m_outputMem->virt_addr, out_mem_sz);
    if (m_totalRecoveries > 0)
        fprintf(stderr, "[rknn] reinit complete — NPU context restored (recovery #%d)\n",
                m_totalRecoveries);
    else
        fprintf(stderr, "[rknn] ready — output FloatMat(%d, %d)  scale=%.6f zp=%d\n",
                m_outW, m_outH, m_outScale, m_outZp);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  infer
// ─────────────────────────────────────────────────────────────────────────────

bool RknnInference::infer(const float* inputCHW, FloatMat& out)
{
    if (!m_ctx && m_readyToInit) {
        if (!lazyInitCtx()) return false;
    }
    if (!m_ctx || !m_inputMem || !m_outputMem) return false;

    auto ctx = static_cast<rknn_context>(m_ctx);

    // Convert float32 CHW [0,1] → uint8 HWC [0,255] directly into DMA buffer.
    // If w_stride > inputW, pad each row to the stride boundary (zero-filled
    // by rknn_create_mem, so only write the active pixels).
    {
        auto* dst     = static_cast<uint8_t*>(m_inputMem->virt_addr);
        const int pix = m_inputH * m_inputW;
        const int stride3 = m_inStride * 3;      // bytes per row in DMA buffer
        const int width3  = m_inputW  * 3;       // bytes per row of active data

        for (int h = 0; h < m_inputH; ++h) {
            uint8_t* row = dst + h * stride3;
            for (int w = 0; w < m_inputW; ++w) {
                for (int c = 0; c < 3; ++c) {
                    float v = inputCHW[c * pix + h * m_inputW + w] * 255.f;
                    row[w * 3 + c] = static_cast<uint8_t>(
                        v < 0.f ? 0 : v > 255.f ? 255 : static_cast<int>(v));
                }
            }
            (void)width3;  // used implicitly in loop above
        }
    }

    int ret = rknn_run(ctx, nullptr);
    if (ret < 0) {
        m_consecutiveFailures++;
        m_totalRecoveries++;
        fprintf(stderr,
            "[rknn] rknn_run failed: %d  (consecutive=%d  total_recoveries=%d)\n"
            "[rknn] tearing down context — will reinitialise on next frame\n",
            ret, m_consecutiveFailures, m_totalRecoveries);

        // Tear down the current context and DMA buffers, but keep m_modelBytes
        // intact so lazyInitCtx() can recreate everything on the next infer() call.
        // The NPU kernel driver performs a soft reset after each timeout.  Wait
        // briefly before destroy so the driver completes its internal reset before
        // we attempt further DMA operations (rknn_destroy_mem touches the bus).
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (m_outputMem) { rknn_destroy_mem(ctx, m_outputMem); m_outputMem = nullptr; }
        if (m_inputMem)  { rknn_destroy_mem(ctx, m_inputMem);  m_inputMem  = nullptr; }
        rknn_destroy(ctx);
        m_ctx         = 0;
        m_outBuf.clear();
        m_readyToInit = true;   // triggers lazyInitCtx() on next infer()
        return false;
    }
    m_consecutiveFailures = 0; // successful inference — reset streak counter

    // Dequantize INT8 output → float32.
    // Affine: float = (int8 - zp) * scale
    {
        const auto* src = static_cast<const int8_t*>(m_outputMem->virt_addr);
        const float  sc  = m_outScale;
        const int32_t zp = m_outZp;
        for (uint32_t i = 0; i < m_outElems; ++i)
            m_outBuf[i] = (static_cast<int32_t>(src[i]) - zp) * sc;
    }

    out = FloatMat(m_outW, m_outH, m_outBuf.data());
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  deinit
// ─────────────────────────────────────────────────────────────────────────────

void RknnInference::deinit()
{
    if (m_ctx) {
        auto ctx = static_cast<rknn_context>(m_ctx);
        if (m_outputMem) { rknn_destroy_mem(ctx, m_outputMem); m_outputMem = nullptr; }
        if (m_inputMem)  { rknn_destroy_mem(ctx, m_inputMem);  m_inputMem  = nullptr; }
        rknn_destroy(ctx);
        m_ctx = 0;
    }
    m_outBuf.clear();
    m_modelBytes.clear();
}
