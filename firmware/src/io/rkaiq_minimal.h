// rkaiq_minimal.h — minimal forward declarations for the rkaiq ISP engine
//
// Covers only the sysctl API needed for in-process AWB/AE/CCM initialisation.
// Avoids pulling in the full rkaiq header tree (53+ files across uAPI2/,
// common/, algos/, xcore/, …) which has complex cross-directory includes.
//
// Verified against librkaiq.so from:
//   LuckfoxTECH/luckfox-pico  media/isp/release_camera_engine_rkaiq_rv1106_*/
//
// Only used when HAVE_RKAIQ is defined (library found by build-luckfox-mac.sh).

#pragma once
#include <cstdint>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Opaque context handle ─────────────────────────────────────────────────────

struct rk_aiq_sys_ctx_s;
typedef struct rk_aiq_sys_ctx_s rk_aiq_sys_ctx_t;

// ── Return codes (XCamReturn) ──────────────────────────────────────────────────
// XCAM_RETURN_NO_ERROR == 0 is all we check; the rest are for completeness.

typedef enum {
    XCAM_RETURN_NO_ERROR       =  0,
    XCAM_RETURN_BYPASS         =  1,
    XCAM_RETURN_ERROR_FAILED   = -1,
    XCAM_RETURN_ERROR_PARAM    = -2,
    XCAM_RETURN_ERROR_MEM      = -3,
    XCAM_RETURN_ERROR_FILE     = -4,
    XCAM_RETURN_ERROR_ANALYZER = -5,
    XCAM_RETURN_ERROR_ISP      = -6,
    XCAM_RETURN_ERROR_SENSOR   = -7,
    XCAM_RETURN_ERROR_THREAD   = -8,
    XCAM_RETURN_ERROR_IOCTL    = -9,
    XCAM_RETURN_ERROR_UNKNOWN  = -255,
} XCamReturn;

// ── Working mode ─────────────────────────────────────────────────────────────

typedef enum {
    RK_AIQ_WORKING_MODE_NORMAL   = 0,
    RK_AIQ_WORKING_MODE_ISP_HDR2 = 0x10,
    RK_AIQ_WORKING_MODE_ISP_HDR3 = 0x20,
} rk_aiq_working_mode_t;

// ── Callback types (passed as nullptr — declared as void* for simplicity) ─────
// All function pointers are 4 bytes on Cortex-A7, so the exact type doesn't
// affect the ABI when we pass nullptr.

typedef void* rk_aiq_error_cb_t;
typedef void* rk_aiq_metas_cb_t;

// ── sysctl API ────────────────────────────────────────────────────────────────

// Returns the sensor V4L2 entity name bound to a VI video device (e.g. /dev/video0).
// Returns nullptr if the sensor cannot be resolved via the media graph.
const char*
rk_aiq_uapi2_sysctl_getBindedSnsEntNmByVd(const char* vd);

// Initialise rkaiq for a given sensor entity name and IQ file directory.
// err_cb and metas_cb may be nullptr.
// Returns nullptr on failure.
rk_aiq_sys_ctx_t*
rk_aiq_uapi2_sysctl_init(const char*        sns_ent_name,
                          const char*        iq_file_dir,
                          rk_aiq_error_cb_t  err_cb,
                          rk_aiq_metas_cb_t  metas_cb);

// Configure the ISP pipeline for the given capture resolution and working mode.
// Must be called after init() and before start().
XCamReturn
rk_aiq_uapi2_sysctl_prepare(const rk_aiq_sys_ctx_t* ctx,
                             uint32_t                width,
                             uint32_t                height,
                             rk_aiq_working_mode_t   mode);

// Start the AE/AWB/AF/CCM control loops.
XCamReturn
rk_aiq_uapi2_sysctl_start(const rk_aiq_sys_ctx_t* ctx);

// Stop the control loops.  keep_ext_hw_st=false lets rkaiq reset ISP state.
XCamReturn
rk_aiq_uapi2_sysctl_stop(const rk_aiq_sys_ctx_t* ctx, bool keep_ext_hw_st);

// Tear down the rkaiq context.  Must be called after stop().
void
rk_aiq_uapi2_sysctl_deinit(rk_aiq_sys_ctx_t* ctx);

#ifdef __cplusplus
}
#endif
