#include "rknn_infer.h"

#include <rknn_api.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static std::vector<uint8_t> loadFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) return {};
    auto size = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(buf.data()), size);
    return buf;
}

// ─────────────────────────────────────────────────────────────────────────────
//  init
// ─────────────────────────────────────────────────────────────────────────────

bool RknnInference::init(const std::string& modelPath, int inputW, int inputH)
{
    m_inputW = inputW;
    m_inputH = inputH;

    // Load model bytes
    auto model = loadFile(modelPath);
    if (model.empty()) {
        fprintf(stderr, "[rknn] cannot read model: %s\n", modelPath.c_str());
        return false;
    }

    // Initialise RKNN context
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model.data(), static_cast<uint32_t>(model.size()),
                        0, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_init failed: %d\n", ret);
        return false;
    }
    m_ctx = static_cast<uint64_t>(ctx);

    // Query number of inputs/outputs
    rknn_input_output_num io_num{};
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_query IN_OUT_NUM failed: %d\n", ret);
        rknn_destroy(ctx);
        m_ctx = 0;
        return false;
    }
    printf("[rknn] model inputs=%u  outputs=%u\n", io_num.n_input, io_num.n_output);

    // Query output tensor shape (index 0)
    rknn_tensor_attr out_attr{};
    out_attr.index = 0;
    ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &out_attr, sizeof(out_attr));
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_query OUTPUT_ATTR failed: %d\n", ret);
        rknn_destroy(ctx);
        m_ctx = 0;
        return false;
    }

    // Map tensor dims to (h, w) for ncnn::Mat(w, h).
    // Expected shapes after RKNN conversion of yolo11n:
    //   3D: [1, 5, 2100]  →  squeeze batch  →  h=5,  w=2100
    //   2D: [5, 2100]                        →  h=5,  w=2100
    // dims[] is in NCHW order; n_dims gives the count.
    if (out_attr.n_dims == 3) {
        // [batch, h, w] — batch should be 1
        m_outH = static_cast<int>(out_attr.dims[1]);
        m_outW = static_cast<int>(out_attr.dims[2]);
    } else if (out_attr.n_dims == 2) {
        m_outH = static_cast<int>(out_attr.dims[0]);
        m_outW = static_cast<int>(out_attr.dims[1]);
    } else {
        fprintf(stderr, "[rknn] unexpected output tensor dims=%u\n", out_attr.n_dims);
        rknn_destroy(ctx);
        m_ctx = 0;
        return false;
    }

    m_outBuf.resize(static_cast<size_t>(m_outH) * m_outW);

    printf("[rknn] output tensor: n_dims=%u  shape=[", out_attr.n_dims);
    for (uint32_t i = 0; i < out_attr.n_dims; ++i)
        printf("%u%s", out_attr.dims[i], i + 1 < out_attr.n_dims ? "," : "");
    printf("]  → ncnn::Mat(%d, %d)\n", m_outW, m_outH);

    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  infer
// ─────────────────────────────────────────────────────────────────────────────

bool RknnInference::infer(const float* inputCHW, ncnn::Mat& out)
{
    if (!m_ctx) return false;

    auto ctx = static_cast<rknn_context>(m_ctx);

    // Set input — channel-planar float32 matches NCHW directly
    rknn_input inputs[1]{};
    inputs[0].index        = 0;
    inputs[0].type         = RKNN_TENSOR_FLOAT32;
    inputs[0].size         = static_cast<uint32_t>(
                                 m_inputH * m_inputW * 3 * sizeof(float));
    inputs[0].fmt          = RKNN_TENSOR_NCHW;
    inputs[0].buf          = const_cast<float*>(inputCHW);
    inputs[0].pass_through = 0;

    int ret = rknn_inputs_set(ctx, 1, inputs);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_inputs_set failed: %d\n", ret);
        return false;
    }

    ret = rknn_run(ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_run failed: %d\n", ret);
        return false;
    }

    // Retrieve dequantised float32 output
    rknn_output outputs[1]{};
    outputs[0].index      = 0;
    outputs[0].want_float = 1;

    ret = rknn_outputs_get(ctx, 1, outputs, nullptr);
    if (ret < 0) {
        fprintf(stderr, "[rknn] rknn_outputs_get failed: %d\n", ret);
        return false;
    }

    memcpy(m_outBuf.data(), outputs[0].buf,
           m_outBuf.size() * sizeof(float));

    rknn_outputs_release(ctx, 1, outputs);

    // Wrap in ncnn::Mat — external data, no ownership transfer
    out = ncnn::Mat(m_outW, m_outH, static_cast<void*>(m_outBuf.data()),
                    sizeof(float));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  deinit
// ─────────────────────────────────────────────────────────────────────────────

void RknnInference::deinit()
{
    if (m_ctx) {
        rknn_destroy(static_cast<rknn_context>(m_ctx));
        m_ctx = 0;
    }
    m_outBuf.clear();
}
