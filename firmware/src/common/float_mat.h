#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
//  FloatMat — lightweight channel-planar float tensor
//
//  Replaces ncnn::Mat throughout the capture and decoder pipeline.
//  Only the subset of ncnn::Mat's interface that is actually used here is
//  provided.
//
//  Layout: channel-planar (CHW)
//    channel(ci) = data() + ci * h * w   (stride = w floats per row)
//    row(r)      = data() + r * w         (within the first channel / flattened
//                                          view for 2-D tensors)
//
//  Two storage modes:
//    Owned      : storage_ holds the data; ext_ is nullptr.
//    Non-owning : ext_ points to external memory; storage_ is empty.
//                 Used by RknnInference to wrap m_outBuf without copying.
//                 The caller must ensure the external buffer outlives the mat.
// ─────────────────────────────────────────────────────────────────────────────

struct FloatMat {
    int    w     = 0;   // columns
    int    h     = 0;   // rows
    int    c     = 1;   // channels
    int    dims  = 0;   // 2 = w×h,  3 = w×h×c
    static constexpr size_t elemsize = sizeof(float);

    FloatMat() = default;

    // 2-D owned (w × h)
    FloatMat(int w_, int h_)
        : w(w_), h(h_), c(1), dims(2),
          storage_(static_cast<size_t>(w_ * h_)) {}

    // 3-D owned (w × h × c)
    FloatMat(int w_, int h_, int c_)
        : w(w_), h(h_), c(c_), dims(3),
          storage_(static_cast<size_t>(w_ * h_ * c_)) {}

    // 2-D non-owning — wraps an external float buffer (no allocation)
    FloatMat(int w_, int h_, float* ext)
        : w(w_), h(h_), c(1), dims(2), ext_(ext) {}

    float*       data()       noexcept { return ext_ ? ext_ : storage_.data(); }
    const float* data() const noexcept { return ext_ ? ext_ : storage_.data(); }

    // row(r): pointer to row r in the flattened (h × w) 2-D layout
    float*       row(int r)       noexcept { return data() + r * w; }
    const float* row(int r) const noexcept { return data() + r * w; }

    // channel(ci): pointer to start of channel ci in CHW planar layout
    float*       channel(int ci)       noexcept { return data() + ci * h * w; }
    const float* channel(int ci) const noexcept { return data() + ci * h * w; }

    // fill — only meaningful for owned mats
    void fill(float v) { std::fill(storage_.begin(), storage_.end(), v); }

    // reshape — returns a 2-D non-owning view over the same data
    FloatMat reshape(int newW, int newH) const noexcept {
        FloatMat m;
        m.w    = newW;
        m.h    = newH;
        m.c    = 1;
        m.dims = 2;
        m.ext_ = const_cast<float*>(data());
        return m;
    }

private:
    std::vector<float> storage_;
    float*             ext_ = nullptr;
};
