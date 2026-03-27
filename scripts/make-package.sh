#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  make-package.sh — build yolo_libcamera and assemble the deployment package
#
#  Run from the repository root on the development CM5:
#    bash scripts/make-package.sh
#
#  On success, the package/ directory is ready to copy to a trap board:
#    scp -r package/ trap@trap006.local:~/
#    ssh trap@trap006.local 'sudo bash ~/package/install.sh'
#
#  Output layout inside package/:
#    bin/yolo_libcamera          stripped binary
#    models/yolo11n-320/         ncnn model files
#    config/trap_config.toml     default runtime config
#    systemd/ai-trap.service
#    install.sh
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/buildDir"
PKG_DIR="${REPO_ROOT}/package"
BINARY="yolo_libcamera"
MODEL_SRC="${REPO_ROOT}/firmware/models/yolo11n-320"

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; NC='\033[0m'
info()  { echo -e "${GREEN}[package]${NC} $*"; }
warn()  { echo -e "${YELLOW}[package]${NC} $*"; }
error() { echo -e "${RED}[package]${NC} $*" >&2; exit 1; }

# ── 1. Build ──────────────────────────────────────────────────────────────────

if [ ! -f "${BUILD_DIR}/build.ninja" ]; then
    error "Build directory not configured. Run: meson setup buildDir -Dtarget=libcamera"
fi

info "Building ${BINARY}..."
ninja -C "${BUILD_DIR}" -j"$(nproc)" 2>&1 | tail -5

BINARY_PATH="${BUILD_DIR}/${BINARY}"
[ -f "${BINARY_PATH}" ] || error "Build succeeded but binary not found: ${BINARY_PATH}"

# ── 2. Strip binary ───────────────────────────────────────────────────────────

info "Stripping binary..."
mkdir -p "${PKG_DIR}/bin"
cp "${BINARY_PATH}" "${PKG_DIR}/bin/${BINARY}"
strip "${PKG_DIR}/bin/${BINARY}"
SIZE=$(du -sh "${PKG_DIR}/bin/${BINARY}" | cut -f1)
info "Binary: ${PKG_DIR}/bin/${BINARY}  (${SIZE})"

# ── 3. Copy model files ───────────────────────────────────────────────────────

MODEL_PARAM="${MODEL_SRC}/model.ncnn.param"
MODEL_BIN="${MODEL_SRC}/model.ncnn.bin"

if [ -f "${MODEL_PARAM}" ] && [ -f "${MODEL_BIN}" ]; then
    info "Copying model files..."
    mkdir -p "${PKG_DIR}/models/yolo11n-320"
    cp "${MODEL_PARAM}" "${PKG_DIR}/models/yolo11n-320/"
    cp "${MODEL_BIN}"   "${PKG_DIR}/models/yolo11n-320/"
else
    warn "Model files not found at ${MODEL_SRC}/ — install.sh will warn on the trap."
fi

# ── 4. Verify package structure ───────────────────────────────────────────────

ERRORS=0
WARNINGS=0
fail() { echo -e "${RED}[FAIL]${NC} $*" >&2; ERRORS=$((ERRORS+1)); }
pass() { echo -e "${GREEN}[ OK ]${NC} $*"; }
chk()  { echo -e "${YELLOW}[WARN]${NC} $*"; WARNINGS=$((WARNINGS+1)); }

echo ""
info "── Completeness check ────────────────────────────────────────────────────"

# Required files
for f in \
    "${PKG_DIR}/bin/${BINARY}" \
    "${PKG_DIR}/install.sh" \
    "${PKG_DIR}/systemd/ai-trap.service" \
    "${PKG_DIR}/systemd/camera-overlay.service" \
    "${PKG_DIR}/config/trap_config.toml"
do
    if [ -f "$f" ]; then
        pass "Present:   ${f#"${PKG_DIR}/"}"
    else
        fail "Missing:   ${f#"${PKG_DIR}/"}"
    fi
done

# Binary must be aarch64 ELF — catch wrong-arch builds before deploy
if [ -f "${PKG_DIR}/bin/${BINARY}" ]; then
    ARCH_INFO=$(file "${PKG_DIR}/bin/${BINARY}")
    if echo "${ARCH_INFO}" | grep -q "aarch64\|ARM aarch64"; then
        pass "Arch:      aarch64 ELF"
    else
        fail "Arch:      binary is NOT aarch64 — ${ARCH_INFO}"
    fi
    # RPATH must include /opt/ai-trap/lib so bundled libs are found at runtime
    if objdump -x "${PKG_DIR}/bin/${BINARY}" 2>/dev/null | grep -q "RPATH.*opt/ai-trap/lib"; then
        pass "RPATH:     /opt/ai-trap/lib present"
    else
        chk  "RPATH:     /opt/ai-trap/lib not found — binary may not find bundled libs"
    fi
fi

# Model files — hard error, service will not start without them
if [ -f "${PKG_DIR}/models/yolo11n-320/model.ncnn.param" ] && \
   [ -f "${PKG_DIR}/models/yolo11n-320/model.ncnn.bin" ]; then
    PARAM_SIZE=$(du -sh "${PKG_DIR}/models/yolo11n-320/model.ncnn.param" | cut -f1)
    BIN_SIZE=$(du -sh   "${PKG_DIR}/models/yolo11n-320/model.ncnn.bin"   | cut -f1)
    pass "Model:     param=${PARAM_SIZE}  bin=${BIN_SIZE}"
else
    fail "Model:     model files missing — service will not start"
fi

# camera-overlay.service must load imx708 (default = Unicam 1, the standard CM5 connector)
if grep -q "dtoverlay imx708" "${PKG_DIR}/systemd/camera-overlay.service" 2>/dev/null; then
    pass "Overlay:   camera-overlay.service loads imx708"
else
    fail "Overlay:   camera-overlay.service missing 'dtoverlay imx708'"
fi

# Service file must not depend on camera-overlay (regression guard)
if grep -q "camera-overlay" "${PKG_DIR}/systemd/ai-trap.service" 2>/dev/null; then
    fail "Service:   ai-trap.service still references camera-overlay"
else
    pass "Service:   no camera-overlay dependency"
fi

# StartLimitIntervalSec must be in [Unit] not [Service] — systemd on Trixie ignores it in [Service]
# and the default limit causes reboot after 5 rapid failures
if awk '/^\[Service\]/,/^\[/' "${PKG_DIR}/systemd/ai-trap.service" | grep -q "StartLimitIntervalSec"; then
    fail "Service:   StartLimitIntervalSec is in [Service] section — must be in [Unit]"
else
    pass "Service:   StartLimitIntervalSec placement correct"
fi

# Service file must set XDG_RUNTIME_DIR (IPA proxy needs it — crash without it)
if grep -q "XDG_RUNTIME_DIR" "${PKG_DIR}/systemd/ai-trap.service" 2>/dev/null; then
    pass "Service:   XDG_RUNTIME_DIR set"
else
    fail "Service:   XDG_RUNTIME_DIR missing — IPA proxy will crash"
fi

# Service file must point IPA to system modules (bundled IPA causes kernel panic on CM5)
if grep -q "LIBCAMERA_IPA_MODULE_PATH" "${PKG_DIR}/systemd/ai-trap.service" 2>/dev/null; then
    pass "Service:   LIBCAMERA_IPA_MODULE_PATH set (system IPA)"
else
    fail "Service:   LIBCAMERA_IPA_MODULE_PATH missing — bundled IPA may cause kernel panic"
fi

echo ""
info "── Package contents ──────────────────────────────────────────────────────"
find "${PKG_DIR}" -not -path "*/luckfox/*" -type f | sort | sed "s|${PKG_DIR}/||" | \
    awk '{printf "  %s\n", $0}'

echo ""
if [ "${ERRORS}" -gt 0 ]; then
    error "Package check FAILED (${ERRORS} error(s), ${WARNINGS} warning(s)) — do not deploy"
elif [ "${WARNINGS}" -gt 0 ]; then
    info  "Package ready with ${WARNINGS} warning(s): ${PKG_DIR}"
else
    info  "Package ready: ${PKG_DIR}"
fi

info ""
info "Deploy to trap:"
info "  scp -r ${PKG_DIR}/ trap@trap006.local:~/"
info "  ssh trap@trap006.local 'sudo bash ~/package/install.sh && sudo reboot'"
