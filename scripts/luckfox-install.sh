#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  luckfox-install.sh
#
#  Stage 2 — Install ai-trap software onto the Luckfox Pico Zero via ADB.
#
#  Run this after luckfox-flash.sh, once the board has booted (ADB visible).
#  WiFi credentials are configured separately by luckfox-wifi-setup.sh.
#
#  Usage
#  ─────
#    bash scripts/luckfox-install.sh           # auto: local build or GitHub Release
#    bash scripts/luckfox-install.sh --local   # force use of local build
#    bash scripts/luckfox-install.sh --release # force download latest GitHub Release
#
#  Package source (in order of preference, unless overridden)
#  ─────────────────────────────────────────────────────────
#    1. Local build   build-luckfox/yolo_v4l2 exists (from build-luckfox-mac.sh)
#    2. GitHub Release  latest ai-trap-luckfox-*.tar.gz from GitHub Releases
#
#  What gets installed on the device
#  ──────────────────────────────────
#    /opt/trap/yolo_v4l2          application binary (RKNN NPU build)
#    /opt/trap/model.rknn         RKNN INT8 model
#    /opt/trap/librknnmrt.so       Rockchip NPU runtime
#    /opt/trap/trap_config.toml   production configuration
#    /opt/trap/crops/             crop output directory
#    /etc/init.d/S50wifi          WiFi AP/station manager
#    /etc/init.d/S99trap          ai-trap service
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR=/opt/trap

GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; NC='\033[0m'
info()  { echo -e "${GREEN}[install]${NC} $*"; }
warn()  { echo -e "${YELLOW}[install]${NC} $*"; }
error() { echo -e "${RED}[install]${NC} $*" >&2; exit 1; }
step()  { echo -e "\n${BOLD}── $* ──${NC}"; }

# ── Prerequisites ─────────────────────────────────────────────────────────────

ensure_homebrew() {
    if command -v brew &>/dev/null; then return; fi
    info "Homebrew not found — installing..."
    /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
    if [[ -x /opt/homebrew/bin/brew ]]; then
        eval "$(/opt/homebrew/bin/brew shellenv)"
    elif [[ -x /usr/local/bin/brew ]]; then
        eval "$(/usr/local/bin/brew shellenv)"
    fi
    command -v brew &>/dev/null || error "Homebrew installation failed."
    info "Homebrew installed ✓"
}

ensure_adb() {
    ensure_homebrew
    if ! command -v adb &>/dev/null; then
        info "adb not found — installing android-platform-tools..."
        brew install android-platform-tools
    fi
    if brew outdated --formula 2>/dev/null | grep -q "^android-platform-tools"; then
        warn "android-platform-tools update available."
        read -r -p "  Upgrade now? [Y/n] " yn
        [[ "${yn}" =~ ^[Nn]$ ]] || brew upgrade android-platform-tools
    fi
    info "adb $(adb version | head -1) ✓"
}

# ── Package detection ─────────────────────────────────────────────────────────

find_local_package() {
    [[ -f "${REPO_ROOT}/build-luckfox/yolo_v4l2" ]] && \
    [[ -f "${REPO_ROOT}/firmware/models/yolo11n-320/model.rknn" ]]
}

find_librknnmrt() {
    # Check the Docker build cache first, then the build output dir
    local candidates=(
        "${HOME}/.cache/ai-trap/luckfox/sysroot/usr/lib/librknnmrt.so"
        "${REPO_ROOT}/build-luckfox/librknnmrt.so"
    )
    for p in "${candidates[@]}"; do
        [[ -f "${p}" ]] && echo "${p}" && return 0
    done
    return 1
}

get_github_release() {
    local tmpdir="$1"

    local remote_url
    remote_url="$(git -C "${REPO_ROOT}" remote get-url origin 2>/dev/null || echo "")"
    local repo
    repo="$(echo "${remote_url}" | sed -E 's|.*github\.com[:/]([^/]+/[^/.]+)(\.git)?$|\1|')"

    if [[ -z "${repo}" || "${repo}" == "${remote_url}" ]]; then
        error "Could not determine GitHub repository from git remote.
       Set origin to your GitHub remote, or use --local to use a local build."
    fi

    info "Fetching latest release from github.com/${repo}..."

    local api_url="https://api.github.com/repos/${repo}/releases/latest"
    local release_json
    release_json="$(curl -fsSL "${api_url}" 2>/dev/null)" || \
        error "Could not reach GitHub API. Check your network connection."

    local download_url
    download_url="$(echo "${release_json}" | \
        python3 -c "
import json, sys
r = json.load(sys.stdin)
assets = [a['browser_download_url'] for a in r.get('assets', []) if 'luckfox' in a['name'] and a['name'].endswith('.tar.gz')]
if not assets:
    print('ERROR: no luckfox release asset found', file=sys.stderr)
    sys.exit(1)
print(assets[0])
")" || error "No Luckfox release asset found in latest GitHub Release.
       Create a release by pushing a v* tag, or use --local."

    local version
    version="$(echo "${release_json}" | python3 -c "import json,sys; print(json.load(sys.stdin).get('tag_name','unknown'))")"
    info "Latest release: ${version}"

    local tarball="${tmpdir}/ai-trap-luckfox.tar.gz"
    info "Downloading ${download_url}..."
    curl -fL --progress-bar "${download_url}" -o "${tarball}"

    info "Extracting..."
    tar -xzf "${tarball}" -C "${tmpdir}"
    local extracted
    extracted="$(find "${tmpdir}" -maxdepth 1 -type d -name 'ai-trap-luckfox-*' | head -1)"
    [[ -n "${extracted}" ]] || error "Could not find extracted package directory."
    echo "${extracted}"
}

# ── Wait for ADB ──────────────────────────────────────────────────────────────

wait_for_adb() {
    local timeout=60 elapsed=0
    while ! adb devices 2>/dev/null | grep -q "device$"; do
        [[ ${elapsed} -ge ${timeout} ]] && \
            error "ADB device not found after ${timeout}s. Check the board has booted and USB is connected."
        printf "\r  Waiting for ADB device... %ds" "${elapsed}"
        sleep 2; elapsed=$((elapsed + 2))
    done
    echo
    info "ADB device detected ✓"
}

# ── Push files to device ──────────────────────────────────────────────────────

push_files() {
    local binary="$1"
    local rknn_model="$2"
    local rknnrt="$3"
    local config="$4"
    local s50wifi="$5"

    step "Creating directories on device"
    adb shell "mkdir -p ${INSTALL_DIR}/crops"
    info "  ${INSTALL_DIR}/ ✓"

    step "Pushing binary"
    adb push "${binary}" "${INSTALL_DIR}/yolo_v4l2"
    adb shell "chmod 755 ${INSTALL_DIR}/yolo_v4l2"
    info "  yolo_v4l2 ✓"

    step "Pushing RKNN model"
    adb push "${rknn_model}" "${INSTALL_DIR}/model.rknn"
    local rknn_size
    rknn_size="$(du -h "${rknn_model}" | cut -f1)"
    info "  model.rknn (${rknn_size}) ✓"

    step "Pushing RKNN runtime library"
    adb push "${rknnrt}" "${INSTALL_DIR}/librknnmrt.so"
    info "  librknnmrt.so ✓"

    step "Pushing configuration"
    adb push "${config}" "${INSTALL_DIR}/trap_config.toml"
    info "  trap_config.toml ✓"

    # Generate S99trap with RKNN positional args and LD_LIBRARY_PATH
    local tmp_s99trap
    tmp_s99trap="$(mktemp /tmp/S99trap.XXXXXX)"
    cat > "${tmp_s99trap}" << 'INITEOF'
#!/bin/sh
# /etc/init.d/S99trap — ai-trap RKNN NPU build
#
# Design notes
# ────────────
# S21appinit (RkLunch.sh) starts rkipc at boot, which configures the ISP and
# starts the full camera pipeline.  yolo_v4l2 opens /dev/video11 alongside
# rkipc — the rkisp driver supports multiple readers on the main-path output.
# Do NOT stop rkipc: tearing down the ISP pipeline leaves /dev/video11 with
# no frames and causes V4L2Capture to block forever.
#
# rkaiq_3A_server is NOT started: running rkaiq causes rk_aiq_uapi_sysctl_start
# to reconfigure the ISP hardware in a way that blocks NPU AXI DMA on RV1106
# (NPU interrupt never fires, all inferences time out).  The rkipc-provided ISP
# configuration is used instead — image has a slight green tint without AWB/CCM,
# but detections work correctly.

INSTALL_DIR=/opt/trap
BINARY=$INSTALL_DIR/yolo_v4l2
LOGFILE=/var/log/ai-trap.log
PIDFILE=/var/run/ai-trap.pid

# RKNN runtime is in /opt/trap — add to dynamic linker search path
export LD_LIBRARY_PATH=/opt/trap${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

case "$1" in
  start)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        echo "ai-trap is already running (PID=$(cat $PIDFILE))"
        exit 0
    fi
    echo "Starting ai-trap (RKNN NPU)..."
    cd "$INSTALL_DIR"
    "$BINARY" \
        "$INSTALL_DIR/model.rknn" \
        "$INSTALL_DIR/detections.db" \
        "$INSTALL_DIR/crops" \
        /dev/video11 \
        >> "$LOGFILE" 2>&1 &
    echo $! > "$PIDFILE"
    echo "ai-trap started (PID=$!)"
    ;;
  stop)
    echo "Stopping ai-trap..."
    if [ -f "$PIDFILE" ]; then
        PID="$(cat "$PIDFILE")"
        if kill "$PID" 2>/dev/null; then
            echo "stopped $BINARY (pid $PID)"
            rm -f "$PIDFILE"
        else
            if killall yolo_v4l2 2>/dev/null; then
                echo "stopped $BINARY (by name)"
            else
                echo "no $BINARY found; none killed"
            fi
            rm -f "$PIDFILE"
        fi
    else
        if killall yolo_v4l2 2>/dev/null; then
            echo "stopped $BINARY (no pidfile, killed by name)"
        else
            echo "no $BINARY found; none killed"
        fi
    fi
    ;;
  restart)
    "$0" stop; sleep 1; "$0" start
    ;;
  status)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        echo "ai-trap is running (PID=$(cat $PIDFILE))"
    else
        echo "ai-trap is not running"
        exit 1
    fi
    ;;
  *)
    echo "Usage: $0 {start|stop|restart|status}"
    exit 1
    ;;
esac
exit 0
INITEOF

    step "Pushing init scripts"
    adb push "${s50wifi}"     /etc/init.d/S50wifi
    adb push "${tmp_s99trap}" /etc/init.d/S99trap
    adb shell "chmod 755 /etc/init.d/S50wifi /etc/init.d/S99trap"
    rm -f "${tmp_s99trap}"
    info "  S50wifi + S99trap ✓"

    step "Verifying installation"
    adb shell "ls -lh ${INSTALL_DIR}/"
}

# ── Main ──────────────────────────────────────────────────────────────────────

MODE="${1:-auto}"

step "Checking prerequisites"
ensure_adb

step "Waiting for ADB device"
wait_for_adb

TMPDIR_PKG="$(mktemp -d)"
trap 'rm -rf "${TMPDIR_PKG}"' EXIT

if [[ "${MODE}" == "--local" ]] || \
   { [[ "${MODE}" == "auto" ]] && find_local_package; }; then

    info "Using local build from build-luckfox/"

    if [[ ! -f "${REPO_ROOT}/build-luckfox/yolo_v4l2" ]]; then
        error "Binary not found. Build first: bash scripts/build-luckfox-mac.sh"
    fi
    if [[ ! -f "${REPO_ROOT}/firmware/models/yolo11n-320/model.rknn" ]]; then
        error "model.rknn not found. Convert first:
       bash firmware/models/scripts/convert-rknn.sh \\
           firmware/models/yolo11n-320/model.onnx \\
           firmware/models/yolo11n-320/model.rknn \\
           firmware/models/calibration"
    fi

    LIBRKNNRT="$(find_librknnmrt)" || \
        error "librknnmrt.so not found in build cache.
       Run: bash scripts/build-luckfox-mac.sh  (it fetches librknnrt automatically)"

    push_files \
        "${REPO_ROOT}/build-luckfox/yolo_v4l2" \
        "${REPO_ROOT}/firmware/models/yolo11n-320/model.rknn" \
        "${LIBRKNNRT}" \
        "${REPO_ROOT}/package/luckfox/trap_config.toml" \
        "${REPO_ROOT}/package/luckfox/S50wifi"

elif [[ "${MODE}" == "--release" ]] || [[ "${MODE}" == "auto" ]]; then

    PKG_DIR="$(get_github_release "${TMPDIR_PKG}")"

    [[ -f "${PKG_DIR}/model.rknn" ]] || \
        error "model.rknn not found in release package. Use --local instead."
    [[ -f "${PKG_DIR}/librknnmrt.so" ]] || \
        error "librknnmrt.so not found in release package. Use --local instead."

    push_files \
        "${PKG_DIR}/yolo_v4l2" \
        "${PKG_DIR}/model.rknn" \
        "${PKG_DIR}/librknnmrt.so" \
        "${PKG_DIR}/trap_config.toml" \
        "${PKG_DIR}/S50wifi"

else
    error "Unknown option: ${MODE}
       Usage: $0 [--local | --release]"
fi

echo
echo "─────────────────────────────────────────────────────"
info "Software installation complete."
echo
echo "Next step — configure WiFi:"
echo "  bash scripts/luckfox-wifi-setup.sh \"YourSSID\" \"YourPassword\""
echo "─────────────────────────────────────────────────────"
