#pragma once

#include <cmath>
#include <vector>
#include <cstdint>

#include "float_mat.h"

// ─────────────────────────────────────────────────────────────────────────────
//  Detection  — one bounding box from one inference pass
// ─────────────────────────────────────────────────────────────────────────────
struct Detection {
    float x1, y1, x2, y2;   // corners in original image pixels
    float confidence;        // class score (sigmoid-activated)
    int   classId;           // index into your class name table
};

// ─────────────────────────────────────────────────────────────────────────────
//  YoloFormat  — controls which decoder path is used
//
//  Auto        — heuristic based on tensor shape + score sampling.
//                Reliable for multi-class models.  For single-class models
//                both formats produce h=5, so score sampling is used:
//                scores > 1.0 → Format A (raw logits); ≤ 1.0 → Format B.
//  AnchorGrid  — force Format A (YOLO11 default YOLO11 export)
//  EndToEnd    — force Format B (NMS baked into model)
//  DFL         — RKNN stripped-DFL format (RV1106 NPU)
//                h = 4*reg_max + nc (e.g. 65 for single-class, reg_max=16)
//                rows 0..4*reg_max-1  : raw DFL regression logits
//                rows 4*reg_max..end  : sigmoid class scores (pre-applied)
//                C++ decoder performs softmax-weighted-sum + anchor grid decode.
// ─────────────────────────────────────────────────────────────────────────────
enum class YoloFormat { Auto, AnchorGrid, EndToEnd, DFL };

// ─────────────────────────────────────────────────────────────────────────────
//  DecoderConfig
// ─────────────────────────────────────────────────────────────────────────────
struct DecoderConfig {
    float      confThresh    = 0.45f;            // minimum score to keep
    float      nmsThresh     = 0.45f;            // IoU threshold for NMS (Format A only)
    int        numClasses    = 80;               // set to match your model
    int        modelWidth    = 640;              // must match the YOLO11 export imgsz
    int        modelHeight   = 640;
    YoloFormat format        = YoloFormat::Auto; // override auto-detection if needed

    // Set true when the model already applies sigmoid to class scores internally
    // (i.e. a Sigmoid layer appears before the final output concat in the .param).
    // Prevents the decoder from applying sigmoid a second time (double-sigmoid
    // compresses all scores toward 0.5 and masks real confidence levels).
    bool preAppliedSigmoid  = false;

    // ── Box sanity filters (applied in original image pixel space) ────────────
    // Detections failing any filter are dropped silently before NMS.
    float minBoxWidth    = 20.f;   // pixels — reject tiny slivers
    float minBoxHeight   = 20.f;
    float maxAspectRatio =  5.f;   // max(w/h, h/w) — insects are not needles
    float maxBoxAreaRatio = 0.15f; // box_area / frame_area — reject region-sized detections
};

// ─────────────────────────────────────────────────────────────────────────────
//  YoloDecoder
//
//  Decodes raw YOLO output tensors from YOLO11n into detections.
//
//  FORMAT A  anchor-grid  (YOLO11n default YOLO11 export)
//    dims=2,  h = 4 + numClasses,  w = numAnchors (~2100 for 320 input)
//    Column i:  rows 0-3 = cx,cy,bw,bh (model pixels)
//               rows 4+  = raw logit class scores (sigmoid applied in decoder)
//    NMS applied after decoding.
//
//  FORMAT B  end-to-end  (NMS baked into model at export time)
//    dims=2,  minor dimension = 4 + numClasses + (numClasses>1 ? 1 : 0)
//    Normal:     h=numDets, w=minor  → row i: [x1,y1,x2,y2,score(,cls)]
//    Transposed: h=minor, w=numDets
//    Scores are post-sigmoid [0,1].  NMS NOT applied in decoder.
//
//  A dims=3 batch tensor is automatically squeezed before dispatch.
// ─────────────────────────────────────────────────────────────────────────────
class YoloDecoder {
public:
    explicit YoloDecoder(const DecoderConfig& cfg = {});

    // Decode a raw YOLO output tensor.
    // srcW/srcH   — original camera frame dimensions in pixels
    // scale       — letterbox scale factor from preprocessing
    // padLeft/Top — letterbox padding in model pixels
    // Returns detections in original image pixel coordinates.
    std::vector<Detection> decode(
        const FloatMat& out,
        int   srcW,    int srcH,
        float scale,
        int   padLeft, int padTop) const;

    const DecoderConfig& config() const { return m_cfg; }
    void setConfig(const DecoderConfig& cfg) { m_cfg = cfg; }

    // Print tensor shape and sample rows — useful when verifying a new export.
    void debugTensor(const FloatMat& out, int maxRows = 5) const;

private:
    DecoderConfig m_cfg;

    std::vector<Detection> dispatch(
        const FloatMat& out,
        int srcW, int srcH, float scale, int padLeft, int padTop) const;

    std::vector<Detection> decodeAnchorGrid(
        const FloatMat& out,
        int srcW, int srcH, float scale, int padLeft, int padTop) const;

    std::vector<Detection> decodeEndToEnd(
        const FloatMat& out,
        int srcW, int srcH, float scale, int padLeft, int padTop) const;

    // RKNN stripped-DFL format (RV1106): raw DFL logits + pre-sigmoid class scores.
    // h = 4*reg_max + numClasses,  w = numAnchors.
    std::vector<Detection> decodeDFL(
        const FloatMat& out,
        int srcW, int srcH, float scale, int padLeft, int padTop) const;

    std::vector<Detection> nms(std::vector<Detection> dets) const;

    static float iou(const Detection& a, const Detection& b);
    static float clampf(float v, float lo, float hi);
    static float unpad(float coord, int pad, float scale);
};
