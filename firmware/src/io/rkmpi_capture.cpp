#include "rkmpi_capture.h"
#include "imgproc.h"
#include "float_mat.h"

// RKMPI headers (from Luckfox SDK: media/rockit/rockit/mpi/sdk/include/)
#include <rk_mpi_sys.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vpss.h>
#include <rk_mpi_venc.h>
#include <rk_mpi_mb.h>
#include <rk_comm_vi.h>
#include <rk_comm_vpss.h>
#include <rk_comm_venc.h>
#include <rk_comm_video.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>

// ── VPSS channel assignments ──────────────────────────────────────────────────
static constexpr VPSS_GRP  VPSS_GRP_ID   = 0;
static constexpr VPSS_CHN  VPSS_CHN_INF  = 0;  // inference resolution (320×320)
static constexpr VPSS_CHN  VPSS_CHN_STR  = 1;  // stream res (640×480) → bound to VENC
static constexpr VPSS_CHN  VPSS_CHN_FULL = 2;  // full-res passthrough (1920×1080)

// ── VENC channel ──────────────────────────────────────────────────────────────
static constexpr int       VENC_CHN_ID   = 0;  // MJPEG hardware encoder

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

static void check(RK_S32 ret, const char* what)
{
    if (ret != RK_SUCCESS) {
        char msg[256];
        snprintf(msg, sizeof(msg), "[rkmpi] %s failed: 0x%x", what, (unsigned)ret);
        throw std::runtime_error(msg);
    }
}

// Copy compact NV12 out of an MB block into a std::vector.
// MB blocks may have stride padding; we de-stride to width-exact layout.
static void mbToNv12(void* virt, int width, int height, std::vector<uint8_t>& out)
{
    // VIDEO_FRAME_INFO_S doesn't expose stride directly here; VPSS sets it to
    // the next 16-byte alignment of width.  For safety we read via virt_addr
    // and treat it as compact (width bytes/row) — the VPSS output for
    // COMPRESS_MODE_NONE / RK_FMT_YUV420SP is stride == width when width is
    // already 16-aligned (320 and 640 both are).
    const size_t sz = static_cast<size_t>(width * height * 3 / 2);
    out.resize(sz);
    memcpy(out.data(), virt, sz);
}

// ─────────────────────────────────────────────────────────────────────────────
//  open — RKMPI init + VI + VPSS + bind
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::open(const RkmpiConfig& cfg)
{
    m_cfg = cfg;

    // Global RKMPI init — idempotent if already called by another component,
    // but since we stopped rkipc this should be the first caller.
    check(RK_MPI_SYS_Init(), "RK_MPI_SYS_Init");

    initVI();
    initVPSS();
    bindVItoVPSS();
    initVENC();

    fprintf(stderr, "[rkmpi] pipeline ready  VI %dx%d → VPSS inf=%dx%d str=%dx%d(VENC) full=%dx%d\n",
            cfg.captureWidth, cfg.captureHeight,
            cfg.modelWidth,  cfg.modelHeight,
            cfg.streamWidth, cfg.streamHeight,
            cfg.captureWidth, cfg.captureHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
//  initVI
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::initVI()
{
    const int dev  = m_cfg.viDev;
    const int pipe = m_cfg.viPipe;
    const int chn  = m_cfg.viChn;

    // ── Device ──
    //
    // Zeroed VI_DEV_ATTR_S lets the driver inherit the sensor configuration
    // from the device tree (IMX415 on RV1106 Luckfox Pico Zero).
    // This avoids conflicting with the BSP sensor init while still giving us
    // full control of the downstream pipeline.
    VI_DEV_ATTR_S devAttr{};
    RK_S32 ret = RK_MPI_VI_GetDevAttr(dev, &devAttr);
    if (ret != RK_SUCCESS) {
        memset(&devAttr, 0, sizeof(devAttr));
        check(RK_MPI_VI_SetDevAttr(dev, &devAttr), "VI_SetDevAttr");
    }
    check(RK_MPI_VI_EnableDev(dev), "VI_EnableDev");

    VI_DEV_BIND_PIPE_S bindPipe{};
    bindPipe.u32Num     = 1;
    bindPipe.PipeId[0]  = pipe;
    check(RK_MPI_VI_SetDevBindPipe(dev, &bindPipe), "VI_SetDevBindPipe");

    // ── Pipe ──
    check(RK_MPI_VI_StartPipe(pipe), "VI_StartPipe");

    // ── Channel ──
    //
    // u32Depth = 0: VI channel is bound to VPSS — frame ownership passes to
    // the bind pipeline.  Setting depth > 0 in bind mode over-allocates
    // buffers and can stall the pipeline.
    VI_CHN_ATTR_S chnAttr{};
    chnAttr.stIspOpt.u32BufCount    = static_cast<RK_U32>(m_cfg.bufferCount);
    chnAttr.stIspOpt.enMemoryType   = VI_V4L2_MEMORY_TYPE_DMABUF;
    chnAttr.stSize.u32Width         = static_cast<RK_U32>(m_cfg.captureWidth);
    chnAttr.stSize.u32Height        = static_cast<RK_U32>(m_cfg.captureHeight);
    chnAttr.enPixelFormat           = RK_FMT_YUV420SP;   // NV12
    chnAttr.enCompressMode          = COMPRESS_MODE_NONE;
    chnAttr.u32Depth                = 0;   // bind mode — do not set > 0
    check(RK_MPI_VI_SetChnAttr(pipe, chn, &chnAttr), "VI_SetChnAttr");
    check(RK_MPI_VI_EnableChn(pipe, chn), "VI_EnableChn");

    fprintf(stderr, "[rkmpi] VI dev=%d pipe=%d chn=%d  %dx%d NV12 bufs=%d\n",
            dev, pipe, chn,
            m_cfg.captureWidth, m_cfg.captureHeight, m_cfg.bufferCount);
}

// ─────────────────────────────────────────────────────────────────────────────
//  initVPSS
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::initVPSS()
{
    // ── Group ──
    VPSS_GRP_ATTR_S grpAttr{};
    grpAttr.u32MaxW         = static_cast<RK_U32>(m_cfg.captureWidth);
    grpAttr.u32MaxH         = static_cast<RK_U32>(m_cfg.captureHeight);
    grpAttr.enPixelFormat   = RK_FMT_YUV420SP;
    grpAttr.enCompressMode  = COMPRESS_MODE_NONE;
    // Frame rate -1/-1 = passthrough (no throttling).
    grpAttr.stFrameRate.s32SrcFrameRate = -1;
    grpAttr.stFrameRate.s32DstFrameRate = -1;
    check(RK_MPI_VPSS_CreateGrp(VPSS_GRP_ID, &grpAttr), "VPSS_CreateGrp");

    // ── Channel 0 — inference (320×320) ──
    VPSS_CHN_ATTR_S infAttr{};
    infAttr.enChnMode       = VPSS_CHN_MODE_USER;
    infAttr.enDynamicRange  = DYNAMIC_RANGE_SDR8;
    infAttr.enPixelFormat   = RK_FMT_YUV420SP;
    infAttr.enCompressMode  = COMPRESS_MODE_NONE;
    infAttr.u32Width        = static_cast<RK_U32>(m_cfg.modelWidth);
    infAttr.u32Height       = static_cast<RK_U32>(m_cfg.modelHeight);
    infAttr.u32Depth        = 1;   // allow GetChnFrame
    infAttr.stFrameRate.s32SrcFrameRate = -1;
    infAttr.stFrameRate.s32DstFrameRate = -1;
    // Aspect ratio: letterbox with grey (114) fill to preserve sensor aspect.
    infAttr.stAspectRatio.enMode           = ASPECT_RATIO_MANUAL;
    infAttr.stAspectRatio.stVideoRect.s32X = 0;
    infAttr.stAspectRatio.stVideoRect.s32Y = 0;
    infAttr.stAspectRatio.stVideoRect.u32Width  = static_cast<RK_U32>(m_cfg.modelWidth);
    infAttr.stAspectRatio.stVideoRect.u32Height = static_cast<RK_U32>(m_cfg.modelHeight);
    infAttr.stAspectRatio.u32BgColor = 0x727272;  // RGB(114,114,114) — YOLO letterbox grey
    check(RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_INF, &infAttr), "VPSS_SetChnAttr(inf)");
    check(RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_INF), "VPSS_EnableChn(inf)");

    // ── Channel 1 — stream (640×480), bound to VENC ──
    // u32Depth=0: frame ownership passes to the bind target (VENC).
    // Do NOT call GetChnFrame on this channel.
    VPSS_CHN_ATTR_S strAttr{};
    strAttr.enChnMode       = VPSS_CHN_MODE_USER;
    strAttr.enDynamicRange  = DYNAMIC_RANGE_SDR8;
    strAttr.enPixelFormat   = RK_FMT_YUV420SP;
    strAttr.enCompressMode  = COMPRESS_MODE_NONE;
    strAttr.u32Width        = static_cast<RK_U32>(m_cfg.streamWidth);
    strAttr.u32Height       = static_cast<RK_U32>(m_cfg.streamHeight);
    strAttr.u32Depth        = 0;   // bind mode — VENC consumes frames
    strAttr.stFrameRate.s32SrcFrameRate = -1;
    strAttr.stFrameRate.s32DstFrameRate = -1;
    check(RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_STR, &strAttr), "VPSS_SetChnAttr(str)");
    check(RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_STR), "VPSS_EnableChn(str)");

    // ── Channel 2 — full-res passthrough (1920×1080) for crop saving ──
    VPSS_CHN_ATTR_S fullAttr{};
    fullAttr.enChnMode       = VPSS_CHN_MODE_USER;
    fullAttr.enDynamicRange  = DYNAMIC_RANGE_SDR8;
    fullAttr.enPixelFormat   = RK_FMT_YUV420SP;
    fullAttr.enCompressMode  = COMPRESS_MODE_NONE;
    fullAttr.u32Width        = static_cast<RK_U32>(m_cfg.captureWidth);
    fullAttr.u32Height       = static_cast<RK_U32>(m_cfg.captureHeight);
    fullAttr.u32Depth        = 1;   // GetChnFrame polling
    fullAttr.stFrameRate.s32SrcFrameRate = -1;
    fullAttr.stFrameRate.s32DstFrameRate = -1;
    fullAttr.stAspectRatio.enMode = ASPECT_RATIO_NONE;  // no scaling, passthrough
    check(RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_FULL, &fullAttr), "VPSS_SetChnAttr(full)");
    check(RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_FULL), "VPSS_EnableChn(full)");

    check(RK_MPI_VPSS_StartGrp(VPSS_GRP_ID), "VPSS_StartGrp");

    fprintf(stderr, "[rkmpi] VPSS grp=%d  chn0=%dx%d(inf)  chn1=%dx%d(str→VENC)  chn2=%dx%d(full)\n",
            VPSS_GRP_ID,
            m_cfg.modelWidth,   m_cfg.modelHeight,
            m_cfg.streamWidth,  m_cfg.streamHeight,
            m_cfg.captureWidth, m_cfg.captureHeight);
}

// ─────────────────────────────────────────────────────────────────────────────
//  bindVItoVPSS
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::bindVItoVPSS()
{
    // VI channel → VPSS group input.
    // VPSS input is addressed as chnId=0 regardless of group.
    MPP_CHN_S srcChn{};
    srcChn.enModId  = RK_ID_VI;
    srcChn.s32DevId = m_cfg.viPipe;
    srcChn.s32ChnId = m_cfg.viChn;

    MPP_CHN_S dstChn{};
    dstChn.enModId  = RK_ID_VPSS;
    dstChn.s32DevId = VPSS_GRP_ID;
    dstChn.s32ChnId = 0;

    check(RK_MPI_SYS_Bind(&srcChn, &dstChn), "SYS_Bind(VI→VPSS)");
    fprintf(stderr, "[rkmpi] VI(pipe=%d,chn=%d) → VPSS(grp=%d) bound\n",
            m_cfg.viPipe, m_cfg.viChn, VPSS_GRP_ID);
}

// ─────────────────────────────────────────────────────────────────────────────
//  initVENC — MJPEG hardware encoder bound to VPSS chn1
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::initVENC()
{
    VENC_CHN_ATTR_S attr{};
    attr.stVencAttr.enType          = RK_VIDEO_ID_MJPEG;
    attr.stVencAttr.enPixelFormat   = RK_FMT_YUV420SP;  // NV12 from VPSS
    attr.stVencAttr.u32MaxPicWidth  = static_cast<RK_U32>(m_cfg.streamWidth);
    attr.stVencAttr.u32MaxPicHeight = static_cast<RK_U32>(m_cfg.streamHeight);
    attr.stVencAttr.u32PicWidth     = static_cast<RK_U32>(m_cfg.streamWidth);
    attr.stVencAttr.u32PicHeight    = static_cast<RK_U32>(m_cfg.streamHeight);
    // Output buffer: generous allocation; MJPEG at q75 for 640×480 is ~30–80 KB.
    attr.stVencAttr.u32BufSize      = static_cast<RK_U32>(m_cfg.streamWidth *
                                                           m_cfg.streamHeight * 2);
    attr.stVencAttr.u32StreamBufCnt = 4;
    attr.stVencAttr.bByFrame        = RK_TRUE;  // one GetStream per frame
    attr.stRcAttr.enRcMode          = VENC_RC_MODE_MJPEGFIXQP;
    attr.stRcAttr.stMjpegFixQp.u32Qfactor = static_cast<RK_U32>(m_cfg.jpegQuality);

    check(RK_MPI_VENC_CreateChn(VENC_CHN_ID, &attr), "VENC_CreateChn");
    check(RK_MPI_VENC_StartRecvFrame(VENC_CHN_ID, nullptr), "VENC_StartRecvFrame");

    // Bind VPSS chn1 (stream) → VENC chn0
    MPP_CHN_S src{};
    src.enModId  = RK_ID_VPSS;
    src.s32DevId = VPSS_GRP_ID;
    src.s32ChnId = VPSS_CHN_STR;

    MPP_CHN_S dst{};
    dst.enModId  = RK_ID_VENC;
    dst.s32DevId = 0;
    dst.s32ChnId = VENC_CHN_ID;

    check(RK_MPI_SYS_Bind(&src, &dst), "SYS_Bind(VPSS→VENC)");

    m_vencInitialised = true;
    fprintf(stderr, "[rkmpi] VENC chn=%d  MJPEG %dx%d  Qfactor=%d\n",
            VENC_CHN_ID, m_cfg.streamWidth, m_cfg.streamHeight, m_cfg.jpegQuality);
}

// ─────────────────────────────────────────────────────────────────────────────
//  start / stop / teardown
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::start()
{
    if (m_running.load()) return;
    m_shouldStop.store(false);
    m_running.store(true);
    m_fetchThread    = std::thread(&RkmpiCapture::fetchLoop,    this);
    m_dispatchThread = std::thread(&RkmpiCapture::dispatchLoop, this);
    if (m_vencInitialised)
        m_vencThread = std::thread(&RkmpiCapture::vencLoop, this);
}

void RkmpiCapture::stop()
{
    m_shouldStop.store(true);
    m_running.store(false);
    m_queueCv.notify_all();

    // Stop VENC receive so vencLoop's GetStream call unblocks and exits.
    if (m_vencInitialised)
        RK_MPI_VENC_StopRecvFrame(VENC_CHN_ID);

    if (m_fetchThread.joinable())    m_fetchThread.join();
    if (m_dispatchThread.joinable()) m_dispatchThread.join();
    if (m_vencThread.joinable())     m_vencThread.join();

    teardown();
}

void RkmpiCapture::teardown()
{
    // ── VENC teardown (StopRecvFrame already called in stop()) ────────────────
    if (m_vencInitialised) {
        // Unbind VPSS chn1 → VENC
        MPP_CHN_S vsrc{};
        vsrc.enModId  = RK_ID_VPSS;
        vsrc.s32DevId = VPSS_GRP_ID;
        vsrc.s32ChnId = VPSS_CHN_STR;
        MPP_CHN_S vdst{};
        vdst.enModId  = RK_ID_VENC;
        vdst.s32DevId = 0;
        vdst.s32ChnId = VENC_CHN_ID;
        RK_MPI_SYS_UnBind(&vsrc, &vdst);
        RK_MPI_VENC_DestroyChn(VENC_CHN_ID);
        m_vencInitialised = false;
    }

    // ── Unbind VI → VPSS ──────────────────────────────────────────────────────
    MPP_CHN_S srcChn{};
    srcChn.enModId  = RK_ID_VI;
    srcChn.s32DevId = m_cfg.viPipe;
    srcChn.s32ChnId = m_cfg.viChn;
    MPP_CHN_S dstChn{};
    dstChn.enModId  = RK_ID_VPSS;
    dstChn.s32DevId = VPSS_GRP_ID;
    dstChn.s32ChnId = 0;
    RK_MPI_SYS_UnBind(&srcChn, &dstChn);

    // ── VPSS teardown ─────────────────────────────────────────────────────────
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_FULL);
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_STR);
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_INF);
    RK_MPI_VPSS_StopGrp(VPSS_GRP_ID);
    RK_MPI_VPSS_DestroyGrp(VPSS_GRP_ID);

    // ── VI teardown ───────────────────────────────────────────────────────────
    RK_MPI_VI_DisableChn(m_cfg.viPipe, m_cfg.viChn);
    RK_MPI_VI_StopPipe(m_cfg.viPipe);
    RK_MPI_VI_DisableDev(m_cfg.viDev);

    RK_MPI_SYS_Exit();
    fprintf(stderr, "[rkmpi] pipeline torn down\n");
}

RkmpiCapture::~RkmpiCapture()
{
    if (m_running.load()) stop();
}

// ─────────────────────────────────────────────────────────────────────────────
//  fetchLoop — VPSS GetChnFrame → copy → release → push to queue
//
//  Holds the VPSS buffer only for the memcpy, then releases it immediately.
//  This keeps the VPSS pool free to accept the next VI frame while the
//  dispatch (inference) thread processes the copy.
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::fetchLoop()
{
    uint64_t frameId = 0;

    while (!m_shouldStop.load()) {
        VIDEO_FRAME_INFO_S infFrame{};
        VIDEO_FRAME_INFO_S fullFrame{};

        // Block up to 1 s for the inference-resolution frame.
        RK_S32 ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_INF, &infFrame, 1000);
        if (ret != RK_SUCCESS) {
            if (!m_shouldStop.load())
                fprintf(stderr, "[rkmpi] VPSS_GetChnFrame(inf) timeout/err: 0x%x\n", (unsigned)ret);
            continue;
        }

        // Pull the full-res frame for crops with a short timeout.
        // (chn1 stream is bound to VENC — not fetched here.)
        ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_FULL, &fullFrame, 200);
        bool hasFull = (ret == RK_SUCCESS);

        // Map virtual addresses and copy NV12 data out.
        RawFrame raw;
        raw.frameId     = ++frameId;
        raw.timestampNs = static_cast<uint64_t>(infFrame.stVFrame.u64PTS) * 1000;

        void* infVirt = RK_MPI_MB_Handle2VirAddr(infFrame.stVFrame.pMbBlk);
        mbToNv12(infVirt, m_cfg.modelWidth, m_cfg.modelHeight, raw.nv12_model);

        if (hasFull) {
            void* fullVirt = RK_MPI_MB_Handle2VirAddr(fullFrame.stVFrame.pMbBlk);
            mbToNv12(fullVirt, m_cfg.captureWidth, m_cfg.captureHeight, raw.nv12_fullres);
        }

        // Release VPSS buffers immediately — hardware can reuse them.
        RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_INF, &infFrame);
        if (hasFull)
            RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_FULL, &fullFrame);

        // Push to one-slot queue.  If dispatch thread is still busy, drop
        // the oldest frame (detector latency > 1 frame period).
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queueHasFrame) ++m_droppedFrames;
            m_queue        = std::move(raw);
            m_queueHasFrame = true;
        }
        m_queueCv.notify_one();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  dispatchLoop — pop queue → NV12→RGB → letterbox float CHW → callback
//
//  NV12→RGB at 320×320 costs ~0.3 ms (vs ~6 ms for 1920×1080 in the V4L2
//  path).  The bilinear resize is eliminated — VPSS did it in hardware.
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::dispatchLoop()
{
    // Reusable scratch buffers (allocated once, grown as needed).
    std::vector<uint8_t> rgbBuf;
    std::vector<float>   chwBuf;

    while (!m_shouldStop.load()) {
        RawFrame raw;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this] {
                return m_queueHasFrame || m_shouldStop.load();
            });
            if (m_shouldStop.load()) break;
            raw             = std::move(m_queue);
            m_queueHasFrame = false;
        }

        const int mW = m_cfg.modelWidth;
        const int mH = m_cfg.modelHeight;
        const int pix = mW * mH;

        // ── NV12 → packed RGB uint8 ───────────────────────────────────────────
        rgbBuf.resize(static_cast<size_t>(pix * 3));
        nv12_to_rgb_u8(raw.nv12_model.data(), mW, mH, rgbBuf.data());

        // ── RGB uint8 HWC → float32 CHW [0,1] ────────────────────────────────
        //
        // VPSS handles letterboxing (aspect-ratio fill with grey) so we just
        // normalise — no additional letterbox step needed here.
        // Scale/pad metadata is derived from the VPSS channel config.
        chwBuf.resize(static_cast<size_t>(pix * 3));
        for (int c = 0; c < 3; ++c) {
            float* plane = chwBuf.data() + c * pix;
            for (int i = 0; i < pix; ++i)
                plane[i] = rgbBuf[i * 3 + c] * (1.f / 255.f);
        }

        // Compute letterbox metadata so YoloDecoder can map boxes back to
        // sensor coordinates.  VPSS preserves aspect ratio with the
        // stAspectRatio config — scale = min(modelW/capW, modelH/capH).
        const float scaleX = static_cast<float>(mW) / m_cfg.captureWidth;
        const float scaleY = static_cast<float>(mH) / m_cfg.captureHeight;
        const float scale  = std::min(scaleX, scaleY);
        const float padLeft = (mW - scale * m_cfg.captureWidth)  * 0.5f;
        const float padTop  = (mH - scale * m_cfg.captureHeight) * 0.5f;

        // ── Build CaptureFrame and fire callback ──────────────────────────────
        CaptureFrame frame;
        frame.frameId      = raw.frameId;
        frame.timestampNs  = raw.timestampNs;
        frame.width        = m_cfg.captureWidth;
        frame.height       = m_cfg.captureHeight;
        frame.scale        = scale;
        frame.padLeft      = padLeft;
        frame.padTop       = padTop;
        frame.nv12_fullres = std::move(raw.nv12_fullres);  // 1920×1080 for crops
        frame.modelInput   = FloatMat(mW, mH, chwBuf.data());

        if (m_frameCb) m_frameCb(frame);
        ++m_frameCount;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  vencLoop — drain MJPEG stream from VENC and fire JpegCallback
//
//  VENC hardware encodes each VPSS chn1 frame (640×480 NV12) into a JPEG.
//  RK_MPI_VENC_GetStream blocks up to the timeout; on success the JPEG data
//  lives inside the MB block until ReleaseStream is called.  We fire the
//  callback while the buffer is still valid, then release.
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::vencLoop()
{
    while (!m_shouldStop.load()) {
        VENC_PACK_S   pack{};
        VENC_STREAM_S stream{};
        stream.pstPack    = &pack;
        stream.u32PackCount = 1;

        RK_S32 ret = RK_MPI_VENC_GetStream(VENC_CHN_ID, &stream, 1000);
        if (ret != RK_SUCCESS) {
            if (!m_shouldStop.load())
                fprintf(stderr, "[rkmpi] VENC_GetStream timeout/err: 0x%x\n", (unsigned)ret);
            continue;
        }

        if (m_jpegCb && stream.u32PackCount > 0 && pack.u32Len > 0) {
            auto* data = static_cast<uint8_t*>(
                RK_MPI_MB_Handle2VirAddr(pack.pMbBlk)) + pack.u32Offset;
            m_jpegCb(data, pack.u32Len);
        }

        RK_MPI_VENC_ReleaseStream(VENC_CHN_ID, &stream);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  printStats
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::printStats() const
{
    fprintf(stderr, "[rkmpi] frames=%-6llu  dropped=%-4llu\n",
            (unsigned long long)m_frameCount,
            (unsigned long long)m_droppedFrames);
}
