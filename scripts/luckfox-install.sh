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
#    bash scripts/luckfox-install.sh              # auto: local build or GitHub Release
#    bash scripts/luckfox-install.sh --local      # force use of local build
#    bash scripts/luckfox-install.sh --release    # force download latest GitHub Release
#    bash scripts/luckfox-install.sh --rknpu-only # push rknpu.ko only (no app reinstall)
#
#  Package source (in order of preference, unless overridden)
#  ─────────────────────────────────────────────────────────
#    1. Local build   build-luckfox/yolo_rkmpi exists (from build-luckfox-mac.sh)
#    2. GitHub Release  latest ai-trap-luckfox-*.tar.gz from GitHub Releases
#
#  What gets installed on the device
#  ──────────────────────────────────
#    /opt/trap/yolo_rkmpi         application binary (RKMPI+VPSS+RKNN build)
#    /opt/trap/model.rknn         RKNN INT8 model
#    /opt/trap/librknnmrt.so      Rockchip NPU runtime
#    /opt/trap/trap_config.toml   production configuration
#    /opt/trap/crops/             crop output directory
#    /etc/init.d/S50wifi          WiFi AP/station manager
#    /etc/init.d/S99trap          ai-trap service (stops rkipc, uses RKMPI pipeline)
#    /oem/usr/ko/rknpu.ko         Rockchip NPU kernel module (if --rknpu-only or --with-rknpu)
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
    [[ -f "${REPO_ROOT}/build-luckfox/yolo_rkmpi" ]] && \
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
    adb push "${binary}" "${INSTALL_DIR}/yolo_rkmpi"
    adb shell "chmod 755 ${INSTALL_DIR}/yolo_rkmpi"
    info "  yolo_rkmpi ✓"

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
# /etc/init.d/S99trap — ai-trap RKMPI+VPSS+RKNN build
#
# Design notes
# ────────────
# RKMPI (rockit) requires exclusive access to the ISP — it calls VI_EnableChn
# which triggers ispStreamOn internally.  rkipc (started by S21appinit /
# RkLunch.sh at boot) holds the VI device open and will conflict.  We stop
# rkipc before starting yolo_rkmpi, and restart it on stop.
#
# rkaiq ISP engine (--no-rkaiq flag): rkaiq in-process (librkaiq.so) has been
# disabled because it causes the ISP to claim full AXI bus ownership, starving
# the NPU DMA completely (irq status 0x0 after 6.5 s; all inference times out).
# Root cause: RV1106 ISP has hard-coded highest NOC bus priority; no QoS
# configuration is exposed in sysfs.  The same librkaiq.so is on /oem/usr/lib
# (OEM version = GitHub version, same git hash 25bd14e), ruling out version
# mismatch.  Software WB correction (4.57 R / 1.77 G / 2.95 B) is applied
# instead.  Remove --no-rkaiq to re-enable rkaiq if a bus QoS fix is found.
#
# bypass_irq_handler=1: switches the NPU from interrupt-driven to polling mode.
# On RV1106, the ISP DMA interrupt can starve the NPU completion interrupt
# (IRQ 37) at Conv:/model.8 (task 85 of 113), causing a 6-second rknn_run
# timeout.  Polling bypasses the interrupt path entirely.  Still needed even
# without rkaiq for robustness against any ISP/CIF DMA interference.

INSTALL_DIR=/opt/trap
BINARY=$INSTALL_DIR/yolo_rkmpi
LOGFILE=/var/log/ai-trap.log
PIDFILE=/var/run/ai-trap.pid

# RKNN runtime (librknnmrt.so) is in /opt/trap.
# librockit.so is in /oem/usr/lib — add both to the search path.
export LD_LIBRARY_PATH=/opt/trap:/oem/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}

case "$1" in
  start)
    if [ -f "$PIDFILE" ] && kill -0 "$(cat $PIDFILE)" 2>/dev/null; then
        echo "ai-trap is already running (PID=$(cat $PIDFILE))"
        exit 0
    fi
    echo "Starting ai-trap (RKMPI+RKNN)..."
    # Stop rkipc — RKMPI needs exclusive ISP/VI access.
    /etc/init.d/S21appinit stop 2>/dev/null || true
    sleep 1
    # Reload the NPU driver to recover from any prior SIGKILL that arrived
    # during rknn_run — SIGKILL during inference corrupts rknpu driver state
    # and causes rknn_init to fail on the next launch.  rmmod/insmod is fast
    # (~100 ms) and safe to run unconditionally on every start.
    rmmod rknpu 2>/dev/null || true
    insmod /oem/usr/ko/rknpu.ko
    # Switch NPU to polling mode — prevents ISP DMA from starving the NPU
    # completion interrupt (IRQ 37) at Conv:/model.8 (task 85 of 113).
    echo 1 > /sys/module/rknpu/parameters/bypass_irq_handler
    cd "$INSTALL_DIR"
    "$BINARY" --no-rkaiq \
        "$INSTALL_DIR/model.rknn" \
        "$INSTALL_DIR/detections.db" \
        "$INSTALL_DIR/crops" \
        >> "$LOGFILE" 2>&1 &
    echo $! > "$PIDFILE"
    echo "ai-trap started (PID=$!)"
    ;;
  stop)
    echo "Stopping ai-trap..."
    # Kill ALL yolo_rkmpi instances (pidfile may be stale if a previous stop
    # failed to wait for the process to exit).
    killall yolo_rkmpi 2>/dev/null || true
    rm -f "$PIDFILE"
    # Wait up to 10 s for SIGTERM to be honoured (process may be inside a
    # 6-second rknn_run timeout and can't handle signals until it returns).
    for i in 1 2 3 4 5 6 7 8 9 10; do
        sleep 1
        pgrep -x yolo_rkmpi > /dev/null 2>&1 || { echo "ai-trap stopped."; break; }
        echo "  waiting for process to exit... (${i}s)"
    done
    # Still alive after 10 s — force-kill.  Driver state will be recovered by
    # rmmod/insmod at the start of the next S99trap start invocation.
    if pgrep -x yolo_rkmpi > /dev/null 2>&1; then
        echo "  process did not exit gracefully — sending SIGKILL"
        killall -9 yolo_rkmpi 2>/dev/null || true
        sleep 1
        echo "ai-trap killed."
    fi
    # NOTE: do NOT restart rkipc here.  rkipc briefly opening and closing the
    # VI/ISP device corrupts the VI driver state, causing 0xa006800e on the next
    # yolo_rkmpi start (VPSS gets no frames).  This device is a dedicated trap —
    # rkipc is not needed when yolo_rkmpi is stopped.
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

# ── rknpu.ko deployment ───────────────────────────────────────────────────────

push_rknpu_ko() {
    local ko_path="$1"

    step "Pushing rknpu.ko to device"

    # Verify vermagic before pushing — a mismatched module will silently fail
    # to load and leave the NPU inaccessible.
    local vermagic
    vermagic="$(strings "${ko_path}" | grep '^vermagic=' || true)"
    info "  built vermagic : ${vermagic}"

    local device_vermagic
    device_vermagic="$(adb shell "strings /oem/usr/ko/rknpu.ko 2>/dev/null | grep '^vermagic='" 2>/dev/null | tr -d '\r' || true)"
    info "  device vermagic: ${device_vermagic}"

    if [[ -n "${device_vermagic}" && "${vermagic}" != "${device_vermagic}" ]]; then
        error "vermagic mismatch — built module won't load on this kernel.
       built : ${vermagic}
       device: ${device_vermagic}
       Rebuild with: bash scripts/build-rknpu-ko.sh"
    fi

    adb push "${ko_path}" /oem/usr/ko/rknpu.ko
    info "  rknpu.ko pushed ✓"

    step "Reloading rknpu kernel module"
    # ai-trap must be stopped before unloading rknpu (it has an open NPU context).
    adb shell "/etc/init.d/S99trap stop 2>/dev/null; sleep 2" 2>/dev/null || true
    adb shell "rmmod rknpu 2>/dev/null || true; sleep 1; insmod /oem/usr/ko/rknpu.ko"
    local drv_ver
    drv_ver="$(adb shell "cat /sys/module/rknpu/version 2>/dev/null" | tr -d '\r' || true)"
    info "  rknpu driver version: ${drv_ver}"
    info "  rknpu.ko reloaded ✓"

    step "Restarting ai-trap"
    adb shell "/etc/init.d/S99trap start"
    info "  ai-trap restarted ✓"
}

# ── Main ──────────────────────────────────────────────────────────────────────

MODE="${1:-auto}"

step "Checking prerequisites"
ensure_adb

step "Waiting for ADB device"
wait_for_adb

# ── rknpu-only mode ───────────────────────────────────────────────────────────

if [[ "${MODE}" == "--rknpu-only" ]]; then
    RKNPU_KO="${REPO_ROOT}/build-luckfox/rknpu.ko"
    [[ -f "${RKNPU_KO}" ]] || \
        error "rknpu.ko not found. Build first: bash scripts/build-rknpu-ko.sh"
    push_rknpu_ko "${RKNPU_KO}"
    echo
    echo "─────────────────────────────────────────────────────"
    info "rknpu kernel module updated."
    echo "─────────────────────────────────────────────────────"
    exit 0
fi

# ── Full application install ──────────────────────────────────────────────────

TMPDIR_PKG="$(mktemp -d)"
trap 'rm -rf "${TMPDIR_PKG}"' EXIT

if [[ "${MODE}" == "--local" ]] || \
   { [[ "${MODE}" == "auto" ]] && find_local_package; }; then

    info "Using local build from build-luckfox/"

    if [[ ! -f "${REPO_ROOT}/build-luckfox/yolo_rkmpi" ]]; then
        error "Binary not found. Build first: bash scripts/build-luckfox-mac.sh"
    fi
    if [[ ! -f "${REPO_ROOT}/firmware/models/yolo11n-320/model.rknn" ]]; then
        error "model.rknn not found. Convert first:
       bash firmware/models/scripts/convert-rknn.sh \\
           firmware/models/yolo11n-320/model.rknn \\
           firmware/models/calibration"
    fi

    LIBRKNNRT="$(find_librknnmrt)" || \
        error "librknnmrt.so not found in build cache.
       Run: bash scripts/build-luckfox-mac.sh  (it fetches librknnrt automatically)"

    push_files \
        "${REPO_ROOT}/build-luckfox/yolo_rkmpi" \
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
        "${PKG_DIR}/yolo_rkmpi" \
        "${PKG_DIR}/model.rknn" \
        "${PKG_DIR}/librknnmrt.so" \
        "${PKG_DIR}/trap_config.toml" \
        "${PKG_DIR}/S50wifi"

else
    error "Unknown option: ${MODE}
       Usage: $0 [--local | --release | --rknpu-only]"
fi

# Optionally also push a locally-built rknpu.ko if present alongside the binary.
if [[ -f "${REPO_ROOT}/build-luckfox/rknpu.ko" ]]; then
    echo
    warn "build-luckfox/rknpu.ko found — pushing updated kernel module"
    push_rknpu_ko "${REPO_ROOT}/build-luckfox/rknpu.ko"
fi

echo
echo "─────────────────────────────────────────────────────"
info "Software installation complete."
echo
echo "Next step — configure WiFi:"
echo "  bash scripts/luckfox-wifi-setup.sh \"YourSSID\" \"YourPassword\""
echo "─────────────────────────────────────────────────────"
