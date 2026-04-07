#pragma once

#include <cstdint>

// ─────────────────────────────────────────────────────────────────────────────
//  nv12_to_rgb_u8
//
//  Converts compact NV12 (YUV420sp) to packed RGB uint8 (HWC).
//  BT.601 limited-range:  Y ∈ [16,235],  U/V ∈ [16,240].
//
//  nv12 layout: Y plane (w*h bytes) immediately followed by interleaved UV
//  plane (w*h/2 bytes).  w and h must both be even.
// ─────────────────────────────────────────────────────────────────────────────

static inline void nv12_to_rgb_u8(const uint8_t* nv12, int w, int h, uint8_t* rgb)
{
    const uint8_t* Y  = nv12;
    const uint8_t* UV = nv12 + w * h;

    for (int row = 0; row < h; ++row) {
        const uint8_t* uvRow = UV + (row >> 1) * w;
        for (int col = 0; col < w; ++col) {
            int C = static_cast<int>(Y[row * w + col])  - 16;
            int D = static_cast<int>(uvRow[col & ~1])   - 128;  // U (Cb)
            int E = static_cast<int>(uvRow[(col&~1)+1]) - 128;  // V (Cr)

            int r = (298 * C           + 409 * E + 128) >> 8;
            int g = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int b = (298 * C + 516 * D           + 128) >> 8;

            uint8_t* px = rgb + (row * w + col) * 3;
            px[0] = static_cast<uint8_t>(r < 0 ? 0 : r > 255 ? 255 : r);
            px[1] = static_cast<uint8_t>(g < 0 ? 0 : g > 255 ? 255 : g);
            px[2] = static_cast<uint8_t>(b < 0 ? 0 : b > 255 ? 255 : b);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  bilinear_resize_rgb_u8
//
//  Bilinear resize of packed RGB uint8 (HWC) from srcW×srcH to dstW×dstH.
// ─────────────────────────────────────────────────────────────────────────────

static inline void bilinear_resize_rgb_u8(
    const uint8_t* src, int srcW, int srcH,
    uint8_t*       dst, int dstW, int dstH)
{
    const float xScale = static_cast<float>(srcW) / dstW;
    const float yScale = static_cast<float>(srcH) / dstH;

    for (int dy = 0; dy < dstH; ++dy) {
        float fy  = (dy + 0.5f) * yScale - 0.5f;
        int   sy0 = static_cast<int>(fy);
        int   sy1 = sy0 + 1;
        float wy1 = fy - static_cast<float>(sy0);
        float wy0 = 1.f - wy1;
        if (sy0 < 0)      sy0 = 0;
        if (sy1 >= srcH)  sy1 = srcH - 1;

        for (int dx = 0; dx < dstW; ++dx) {
            float fx  = (dx + 0.5f) * xScale - 0.5f;
            int   sx0 = static_cast<int>(fx);
            int   sx1 = sx0 + 1;
            float wx1 = fx - static_cast<float>(sx0);
            float wx0 = 1.f - wx1;
            if (sx0 < 0)      sx0 = 0;
            if (sx1 >= srcW)  sx1 = srcW - 1;

            for (int ch = 0; ch < 3; ++ch) {
                float v = wy0 * (wx0 * src[(sy0 * srcW + sx0) * 3 + ch] +
                                 wx1 * src[(sy0 * srcW + sx1) * 3 + ch]) +
                          wy1 * (wx0 * src[(sy1 * srcW + sx0) * 3 + ch] +
                                 wx1 * src[(sy1 * srcW + sx1) * 3 + ch]);
                dst[(dy * dstW + dx) * 3 + ch] = static_cast<uint8_t>(v + 0.5f);
            }
        }
    }
}
