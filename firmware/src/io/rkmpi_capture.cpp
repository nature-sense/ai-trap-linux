#include "rkmpi_capture.h"
#include "imgproc.h"
#include "float_mat.h"

// JPEG encoding: prefer libjpeg-turbo (NEON SIMD, ~5ms per frame on Cortex-A7)
// over stb_image_write (~100ms, no SIMD).  libjpeg-turbo is cross-compiled by
// build-luckfox-mac.sh and linked statically — no runtime dependency.
// Fall back to stb if libjpeg is not available (e.g. native/Pi builds).
#if defined(HAVE_LIBJPEG)
#  include <jpeglib.h>
#  include <csetjmp>
#else
#  include "stb_image_write.h"
#endif

// RKMPI headers (from Luckfox SDK: media/rockit/rockit/mpi/sdk/include/)
#include <rk_mpi_mb.h>
#include <rk_mpi_sys.h>
#include <rk_mpi_vi.h>
#include <rk_mpi_vpss.h>
#include <rk_mpi_venc.h>
#include <rk_comm_vi.h>
#include <rk_comm_vpss.h>
#include <rk_comm_venc.h>
#include <rk_comm_video.h>

#include <algorithm>
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
    if (!virt) { out.clear(); return; }
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

    // ── Defensive reset: exit any stale MPI state from a previous process ────
    //
    // When yolo_rkmpi is restarted after a stop/start cycle, rkipc may have run
    // briefly between the two invocations (S99trap stop restarts S21appinit).
    // The rockit userspace IPC state (shared memory, message queues) can be left
    // in an inconsistent state that prevents VI from delivering frames to VPSS
    // (symptom: 0xa006800e on all GetChnFrame calls).
    //
    // Calling RK_MPI_SYS_Exit() before Init() resets the userspace MPI state.
    // It will return an error if MPI is not currently initialised — that is
    // expected and safe to ignore here.
    RK_MPI_SYS_Exit();   // ignore error — expected if MPI not previously init'd

    // ── Static DMA pool pre-allocation ───────────────────────────────────────
    //
    // RK_MPI_MB_SetModPoolConfig() tells the RKMPI allocator exactly how much
    // CMA to reserve per module.  This must be called BEFORE RK_MPI_SYS_Init()
    // so that all DMA buffers are committed in one shot while the ISP is idle.
    //
    // Without this, each module calls mmap() on /dev/mpi individually after the
    // ISP has started streaming.  The CMA allocator then has to migrate pages
    // that the ISP DMA has already pinned, which deadlocks inside the kernel
    // (process enters D-state, unkillable).
    //
    // Sizes must match what each module will request:
    //   VI  : captureWidth × captureHeight × 1.5 (NV12)  × bufferCount
    //   VPSS: not pre-allocated here — VPSS draws from the VI pool via bind
    //   VENC: virW × virH × 2 (generous NV12 equivalent)  × streamBufCnt=4

    const RK_U32 virW = static_cast<RK_U32>((cfg.streamWidth  + 15) & ~15);
    const RK_U32 virH = static_cast<RK_U32>((cfg.streamHeight + 15) & ~15);

    // VI pool: 4 × 1920×1080 NV12
    {
        MB_CONFIG_S mbCfg{};
        mbCfg.astCommPool[0].u64MBSize   = static_cast<RK_U64>(cfg.captureWidth)
                                         * cfg.captureHeight * 3 / 2;
        mbCfg.astCommPool[0].u32MBCnt    = static_cast<RK_U32>(cfg.bufferCount);
        mbCfg.astCommPool[0].enAllocType = MB_ALLOC_TYPE_DMA;
        check(RK_MPI_MB_SetModPoolConfig(MB_UID_VI, &mbCfg), "MB_SetModPoolConfig(VI)");
    }

    if (cfg.enableVenc) {
        MB_CONFIG_S mbCfg{};
        mbCfg.astCommPool[0].u64MBSize   = static_cast<RK_U64>(virW) * virH * 2;
        mbCfg.astCommPool[0].u32MBCnt    = 4;  // streamBufCnt
        mbCfg.astCommPool[0].enAllocType = MB_ALLOC_TYPE_DMA;
        check(RK_MPI_MB_SetModPoolConfig(MB_UID_VENC, &mbCfg), "MB_SetModPoolConfig(VENC)");
    }

    // When VENC is active, reduce VI buffer count 4→2 to free ~6 MB of CMA.
    // VENC mmap() needs contiguous CMA; fewer VI buffers means less migration
    // pressure when the VEPU driver allocates its ring buffer.
    if (cfg.enableVenc) m_cfg.bufferCount = std::min(m_cfg.bufferCount, 2);

    // Global RKMPI init — commits all module pool allocations in one CMA call.
    check(RK_MPI_SYS_Init(), "RK_MPI_SYS_Init");

    initVI();        // configure VI dev/pipe/channel
    startVI();       // VI_EnableChn → ispStreamOn (VENC mmap requires ISP running)
    initVPSS();      // configure VPSS group + channels (CHN_FULL skipped if enableVenc)
    check(RK_MPI_VPSS_StartGrp(VPSS_GRP_ID), "VPSS_StartGrp");
    if (cfg.enableVenc) initVENC();  // VENC after ISP but with reduced CMA pressure
    bindVItoVPSS();

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

    // ── Channel attr (no EnableChn yet — ISP DMA must not start until after
    //    VENC ring buffer is allocated; see startVI()) ──
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

    fprintf(stderr, "[rkmpi] VI dev=%d pipe=%d chn=%d  %dx%d NV12 bufs=%d\n",
            dev, pipe, chn,
            m_cfg.captureWidth, m_cfg.captureHeight, m_cfg.bufferCount);
}

// ─────────────────────────────────────────────────────────────────────────────
//  startVI — enables the VI channel, which triggers ispStreamOn (ISP DMA start)
//
//  Called after VENC_CreateChn so the VEPU driver can allocate its ring buffer
//  from CMA while the ISP is still idle.  EnableChn is the only VI call that
//  touches the ISP DMA engine; everything before it is pure configuration.
// ─────────────────────────────────────────────────────────────────────────────

void RkmpiCapture::startVI()
{
    check(RK_MPI_VI_EnableChn(m_cfg.viPipe, m_cfg.viChn), "VI_EnableChn");
    fprintf(stderr, "[rkmpi] VI streaming started\n");
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
    // Frame rate throttle: when m_cfg.framerate > 0 the VPSS group drops input
    // frames before RGA2 scaling, reducing both RGA2 DMA and downstream NPU DMA.
    // The ISP itself still runs at the sensor native rate (required by rkaiq for
    // stable AE/AWB convergence).  Set to -1/-1 for full passthrough.
    if (m_cfg.framerate > 0) {
        grpAttr.stFrameRate.s32SrcFrameRate = 30;  // sensor native rate
        grpAttr.stFrameRate.s32DstFrameRate = m_cfg.framerate;
    } else {
        grpAttr.stFrameRate.s32SrcFrameRate = -1;
        grpAttr.stFrameRate.s32DstFrameRate = -1;
    }
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

    // ── Channel 1 — stream (640×480) ──
    // enableVenc=true  → depth=0, NV12, bound to VENC (hardware MJPEG)
    // enableVenc=false → depth=1, NV12, polled by softwareJpegLoop.
    //   NV12→RGB is done in CPU (nv12_to_rgb_u8) before JPEG encoding.
    //   Note: RK_FMT_RGB888 is not reliably supported by VPSS on this BSP.
    {
        VPSS_CHN_ATTR_S strAttr{};
        strAttr.enChnMode       = VPSS_CHN_MODE_USER;
        strAttr.enDynamicRange  = DYNAMIC_RANGE_SDR8;
        strAttr.enPixelFormat   = RK_FMT_YUV420SP;   // NV12 — safe on all BSPs
        strAttr.enCompressMode  = COMPRESS_MODE_NONE;
        strAttr.u32Width        = static_cast<RK_U32>(m_cfg.streamWidth);
        strAttr.u32Height       = static_cast<RK_U32>(m_cfg.streamHeight);
        strAttr.u32Depth        = m_cfg.enableVenc ? 0 : 1;   // 0=bind, 1=poll
        strAttr.stFrameRate.s32SrcFrameRate = -1;
        strAttr.stFrameRate.s32DstFrameRate = -1;
        check(RK_MPI_VPSS_SetChnAttr(VPSS_GRP_ID, VPSS_CHN_STR, &strAttr), "VPSS_SetChnAttr(str)");
        check(RK_MPI_VPSS_EnableChn(VPSS_GRP_ID, VPSS_CHN_STR), "VPSS_EnableChn(str)");
    }

    // ── Channel 2 — full-res passthrough (1920×1080) for crop saving ──
    // Skipped when enableVenc=true: the 1920×1080 VPSS channel allocates ~3 MB
    // of CMA-backed DMA buffers.  With VI (4×3.1 MB) + VPSS CHN_FULL already
    // pinning ~15 MB, VENC's mmap() can trigger a CMA migration deadlock.
    // When VENC is active the CropSaver falls back to the inference-resolution
    // frame (CHN_INF, 320×320) — adequate for saving detection thumbnails.
    if (!m_cfg.enableVenc) {
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
    }

    // VPSS_StartGrp activates RGA2 DMA — deferred to open() after VENC is ready.

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
    // VirWidth/VirHeight must be explicitly set — RKMPI rejects u32VirWidth=0.
    // Align to 16 bytes (the minimum RGA2/VENC stride requirement on RV1106).
    const RK_U32 virW = static_cast<RK_U32>((m_cfg.streamWidth  + 15) & ~15);
    const RK_U32 virH = static_cast<RK_U32>((m_cfg.streamHeight + 15) & ~15);

    VENC_CHN_ATTR_S attr{};
    attr.stVencAttr.enType          = RK_VIDEO_ID_MJPEG;
    attr.stVencAttr.enPixelFormat   = RK_FMT_YUV420SP;  // NV12 from VPSS
    attr.stVencAttr.u32MaxPicWidth  = static_cast<RK_U32>(m_cfg.streamWidth);
    attr.stVencAttr.u32MaxPicHeight = static_cast<RK_U32>(m_cfg.streamHeight);
    attr.stVencAttr.u32PicWidth     = static_cast<RK_U32>(m_cfg.streamWidth);
    attr.stVencAttr.u32PicHeight    = static_cast<RK_U32>(m_cfg.streamHeight);
    attr.stVencAttr.u32VirWidth     = virW;
    attr.stVencAttr.u32VirHeight    = virH;
    // Output buffer: generous allocation; MJPEG at q75 for 640×480 is ~30–80 KB.
    attr.stVencAttr.u32BufSize      = virW * virH * 2;
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
    else if (m_jpegCb)
        m_softJpegThread = std::thread(&RkmpiCapture::softwareJpegLoop, this);
}

void RkmpiCapture::stop()
{
    m_shouldStop.store(true);
    m_running.store(false);
    m_queueCv.notify_all();

    // Stop VENC receive so vencLoop's GetStream call unblocks and exits.
    if (m_vencInitialised)
        RK_MPI_VENC_StopRecvFrame(VENC_CHN_ID);

    if (m_fetchThread.joinable())      m_fetchThread.join();
    if (m_dispatchThread.joinable())   m_dispatchThread.join();
    if (m_vencThread.joinable())       m_vencThread.join();
    if (m_softJpegThread.joinable())   m_softJpegThread.join();

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
    if (!m_cfg.enableVenc) RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_FULL);
    RK_MPI_VPSS_DisableChn(VPSS_GRP_ID, VPSS_CHN_STR);  // always enabled
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
            // If the driver returns the error immediately (no blocking) we must
            // throttle the loop so it doesn't spin and starve the rest of the
            // system (makes ADB shell unusable and pegs the CPU).
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Pull the full-res frame for crops with a short timeout.
        // (chn1 stream is bound to VENC — not fetched here.)
        // CHN_FULL is only enabled when VENC is disabled (CMA pressure reduction).
        bool hasFull = false;
        if (!m_cfg.enableVenc) {
            ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_FULL, &fullFrame, 200);
            hasFull = (ret == RK_SUCCESS);
        }

        // Map virtual addresses and copy NV12 data out.
        RawFrame raw;
        raw.frameId     = ++frameId;
        raw.timestampNs = static_cast<uint64_t>(infFrame.stVFrame.u64PTS) * 1000;

        void* infVirt = RK_MPI_MB_Handle2VirAddr(infFrame.stVFrame.pMbBlk);
        // Fall back to pVirAddr[0] if MB_Handle2VirAddr returns null.
        if (!infVirt) infVirt = infFrame.stVFrame.pVirAddr[0];
        mbToNv12(infVirt, m_cfg.modelWidth, m_cfg.modelHeight, raw.nv12_model);

        if (hasFull) {
            void* fullVirt = RK_MPI_MB_Handle2VirAddr(fullFrame.stVFrame.pMbBlk);
            if (!fullVirt) fullVirt = fullFrame.stVFrame.pVirAddr[0];
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
//  softwareJpegLoop — JPEG encoding of the MJPEG stream
//
//  Used when enableVenc=false (hardware VENC bypassed due to mpp_vcodec driver
//  incompatibility on this BSP).  Polls VPSS CHN_STR (640×480 NV12, depth=1),
//  converts NV12→RGB in CPU, then encodes to JPEG.
//
//  Note: RK_FMT_RGB888 output from VPSS was attempted but causes the process
//  to enter D-state on this BSP — RGA2 on RV1106 does not reliably support
//  RGB888 as a VPSS channel output format.  NV12 is used instead.
//
//  Encoding backend (selected at compile time):
//    HAVE_LIBJPEG: libjpeg-turbo with NEON SIMD — ~5–10 ms per 640×480 frame.
//    fallback:     stb_image_write — ~100 ms, no SIMD (6fps ceiling).
// ─────────────────────────────────────────────────────────────────────────────

#if defined(HAVE_LIBJPEG)

// libjpeg-turbo error handler — longjmp on fatal error so we don't abort().
struct JpegErrorMgr {
    jpeg_error_mgr pub;
    jmp_buf        jmpBuf;
};

static void jpegErrorExit(j_common_ptr cinfo)
{
    auto* mgr = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    longjmp(mgr->jmpBuf, 1);
}

// Encode packed RGB888 → JPEG using libjpeg-turbo.
// Returns an empty vector on failure.
static std::vector<uint8_t> encodeJpegTurbo(
    const uint8_t* rgb, int W, int H, int quality)
{
    jpeg_compress_struct cinfo{};
    JpegErrorMgr         jerr{};

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpegErrorExit;

    std::vector<uint8_t> out;

    if (setjmp(jerr.jmpBuf)) {
        jpeg_destroy_compress(&cinfo);
        return {};
    }

    jpeg_create_compress(&cinfo);

    // In-memory destination: libjpeg-turbo allocates the buffer; we copy it.
    unsigned char* jpegBuf = nullptr;
    unsigned long  jpegLen = 0;
    jpeg_mem_dest(&cinfo, &jpegBuf, &jpegLen);

    cinfo.image_width      = static_cast<JDIMENSION>(W);
    cinfo.image_height     = static_cast<JDIMENSION>(H);
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);
    while (cinfo.next_scanline < cinfo.image_height) {
        // Cast away const — libjpeg API requires JSAMPROW (non-const).
        JSAMPROW row = const_cast<JSAMPROW>(
            rgb + static_cast<size_t>(cinfo.next_scanline) * W * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    if (jpegBuf && jpegLen > 0) {
        out.assign(jpegBuf, jpegBuf + jpegLen);
        free(jpegBuf);  // libjpeg-turbo allocates with malloc
    }
    return out;
}

#else  // stb fallback

static void stbJpegCallback(void* ctx, void* data, int size)
{
    auto* vec = static_cast<std::vector<uint8_t>*>(ctx);
    const uint8_t* d = static_cast<const uint8_t*>(data);
    vec->insert(vec->end(), d, d + size);
}

#endif  // HAVE_LIBJPEG

void RkmpiCapture::softwareJpegLoop()
{
    const int W = m_cfg.streamWidth;
    const int H = m_cfg.streamHeight;
    // VPSS CHN_STR outputs NV12. CPU converts to RGB before JPEG encoding.
    const size_t nv12Size = static_cast<size_t>(W * H * 3 / 2);
    const size_t rgbSize  = static_cast<size_t>(W * H * 3);
    // Allocate with 16-byte alignment for libjpeg-turbo NEON SIMD safety.
    // std::vector heap may only guarantee 8-byte alignment; over-allocate by 15
    // and align the working pointer manually.
    std::vector<uint8_t> nv12Buf(nv12Size + 15);
    std::vector<uint8_t> rgbBuf(rgbSize  + 15);
    uint8_t* nv12 = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(nv12Buf.data()) + 15u) & ~15u);
    uint8_t* rgb  = reinterpret_cast<uint8_t*>(
        (reinterpret_cast<uintptr_t>(rgbBuf.data())  + 15u) & ~15u);
    std::vector<uint8_t> jpeg;
    jpeg.reserve(64 * 1024);

#if defined(HAVE_LIBJPEG)
    fprintf(stderr, "[rkmpi] softwareJpegLoop: libjpeg-turbo NEON encoder\n");
#else
    fprintf(stderr, "[rkmpi] softwareJpegLoop: stb_image_write fallback encoder\n");
#endif

    int dbgFrames = 0;  // checkpoint counter — printed once per stage

    while (!m_shouldStop.load()) {
        VIDEO_FRAME_INFO_S frame{};
        RK_S32 ret = RK_MPI_VPSS_GetChnFrame(VPSS_GRP_ID, VPSS_CHN_STR, &frame, 100);
        if (ret != RK_SUCCESS) {
            if (dbgFrames == 0)
                fprintf(stderr, "[str] GetChnFrame err 0x%x\n", (unsigned)ret);
            // Throttle: if driver returns error immediately (doesn't honour the
            // timeout), sleep so we don't spin and starve the system.
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Map virtual address.  On this BSP pVirAddr[0] is set directly by VPSS;
        // fall back to MB_Handle2VirAddr if it is null.
        const auto& vf = frame.stVFrame;
        void* virt = vf.pVirAddr[0]
                   ? vf.pVirAddr[0]
                   : RK_MPI_MB_Handle2VirAddr(vf.pMbBlk);

        if (dbgFrames < 3) {
            // Diagnose NV12 layout: check actual stride and UV plane address.
            // If pVirAddr[1]-pVirAddr[0] != W*H the stride > W and the flat
            // memcpy puts Y data where we expect UV → green colour cast.
            const ptrdiff_t yuvOffset = (vf.pVirAddr[0] && vf.pVirAddr[1])
                ? static_cast<ptrdiff_t>(
                    static_cast<const uint8_t*>(vf.pVirAddr[1]) -
                    static_cast<const uint8_t*>(vf.pVirAddr[0]))
                : -1;
            const uint32_t stride = vf.u32VirWidth ? vf.u32VirWidth : static_cast<uint32_t>(W);

            uint8_t y0 = 0, uv0actual = 0;
            if (virt) {
                const auto* b = static_cast<const uint8_t*>(virt);
                y0 = b[0];
                // Read first UV byte from actual UV plane (via pVirAddr[1]) if available.
                if (vf.pVirAddr[1])
                    uv0actual = static_cast<const uint8_t*>(vf.pVirAddr[1])[0];
            }
            fprintf(stderr,
                "[str] frame=%d virt=%p y0=%02x"
                " stride=%u yuvOff=%ld WxH=%d uvActual=%02x\n",
                dbgFrames, virt, (unsigned)y0,
                (unsigned)stride, (long)yuvOffset, W*H, (unsigned)uv0actual);
            ++dbgFrames;
        }

        // Copy NV12 out (stride-aware) then immediately release the VPSS buffer.
        // pVirAddr[0] = Y plane, pVirAddr[1] = UV plane.  Use the actual UV
        // plane pointer rather than assuming UV is at Y + W*H, which fails when
        // the VPSS output stride > W (e.g. 768 for 256-byte DMA alignment).
        {
            const uint8_t* srcY  = static_cast<const uint8_t*>(virt);
            const uint8_t* srcUV = vf.pVirAddr[1]
                ? static_cast<const uint8_t*>(vf.pVirAddr[1])
                : (srcY ? srcY + static_cast<size_t>(vf.u32VirWidth ? vf.u32VirWidth : static_cast<uint32_t>(W)) * static_cast<size_t>(H) : nullptr);
            const size_t rowStride = vf.u32VirWidth ? static_cast<size_t>(vf.u32VirWidth) : static_cast<size_t>(W);

            if (srcY && srcUV) {
                // Y rows: each stride bytes wide, copy only W bytes per row.
                for (int row = 0; row < H; ++row)
                    memcpy(nv12 + row * W, srcY + row * rowStride, static_cast<size_t>(W));
                // UV rows (H/2 rows, each stride bytes, interleaved U+V pairs).
                for (int row = 0; row < H / 2; ++row)
                    memcpy(nv12 + W * H + row * W, srcUV + row * rowStride, static_cast<size_t>(W));
            }
        }
        RK_MPI_VPSS_ReleaseChnFrame(VPSS_GRP_ID, VPSS_CHN_STR, &frame);

        if (!virt) continue;

        // NV12 → RGB (CPU) → JPEG (libjpeg-turbo NEON or stb fallback)
        nv12_to_rgb_u8(nv12, W, H, rgb);

        // Software white-balance correction.
        // softwareJpegLoop bypasses MjpegStreamer::encodeFrame(), so WB must be
        // applied here.  Gains compensate for ISP running without rkaiq AWB/CCM:
        // IMX415 raw Bayer has a strong green bias and no auto-exposure.
        {
            uint8_t* p         = rgb;
            const uint8_t* end = p + static_cast<size_t>(W * H * 3);
            const float kR = m_cfg.wbR, kG = m_cfg.wbG, kB = m_cfg.wbB;
            while (p < end) {
                float r = p[0] * kR; p[0] = r > 255.f ? 255u : static_cast<uint8_t>(r);
                float g = p[1] * kG; p[1] = g > 255.f ? 255u : static_cast<uint8_t>(g);
                float b = p[2] * kB; p[2] = b > 255.f ? 255u : static_cast<uint8_t>(b);
                p += 3;
            }
        }

        // One-shot: log centre pixel to confirm colour conversion is working.
        static int rgbDbgCount = 0;
        if (rgbDbgCount < 2) {
            const uint8_t* px = rgb + (H/2 * W + W/2) * 3;  // centre pixel
            fprintf(stderr, "[str] rgb-ok frame=%d centre=(%u,%u,%u) nv12[0]=%02x\n",
                    dbgFrames - 1, (unsigned)px[0], (unsigned)px[1], (unsigned)px[2],
                    (unsigned)nv12[0]);
            ++rgbDbgCount;
        }

#if defined(HAVE_LIBJPEG)
        jpeg = encodeJpegTurbo(rgb, W, H, m_cfg.jpegQuality);
#else
        jpeg.clear();
        stbi_write_jpg_to_func(stbJpegCallback, &jpeg, W, H, 3, rgb, m_cfg.jpegQuality);
#endif
        if (!jpeg.empty() && m_jpegCb)
            m_jpegCb(jpeg.data(), jpeg.size());
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
