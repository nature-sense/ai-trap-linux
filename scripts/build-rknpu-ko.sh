#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build-rknpu-ko.sh
#
#  Builds rknpu.ko v0.9.8 for the Luckfox Pico Zero (RV1106G3, kernel 5.10.160)
#  on an Apple Silicon (or Intel) Mac using Docker.
#
#  The Rockchip rknpu driver 0.9.8 (upstream develop-5.10 branch) adds:
#   - rknpu_devfreq.c  — refactored devfreq/OPP/thermal management
#   - rknpu_iommu.c    — full IOMMU multi-domain switching (was ~60 lines, now 544)
#   - rknpu_gem.c      — iommu_switch_domain integration + fixes
#   - Various minor fixes to rknpu_job.c, rknpu_debugger.c, rknpu_reset.c
#
#  The built module is loaded into /oem/usr/ko/rknpu.ko on the device.
#
#  Requirements:
#    Docker Desktop — https://www.docker.com/products/docker-desktop/
#
#  Usage:
#    bash scripts/build-rknpu-ko.sh
#
#  Output:
#    build-luckfox/rknpu.ko
#
#  Cached in ~/.cache/ai-trap/luckfox/:
#    toolchain/       Luckfox ARM cross-compiler
#    kernel-src/      Luckfox BSP kernel 5.10.160 source tree + prepared headers
#  First build downloads ~300 MB kernel source; subsequent builds are fast.
#
#  Deploy:
#    bash scripts/luckfox-install.sh --rknpu-only
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── macOS: re-exec inside Docker ─────────────────────────────────────────────

if [[ "$(uname)" == "Darwin" ]]; then
    HOST_CACHE="${HOME}/.cache/ai-trap/luckfox"
    mkdir -p "${HOST_CACHE}"

    if ! command -v docker &>/dev/null; then
        echo "ERROR: Docker is required." >&2
        echo "       Install Docker Desktop from https://www.docker.com/products/docker-desktop/" >&2
        exit 1
    fi

    echo "==> Launching Ubuntu 22.04 build container (linux/amd64)..."
    echo "    Repo  : ${REPO_ROOT}"
    echo "    Cache : ${HOST_CACHE}"
    echo

    exec docker run --rm \
        --platform linux/amd64 \
        --mount "type=bind,src=${REPO_ROOT},dst=/src" \
        --mount "type=bind,src=${HOST_CACHE},dst=/cache" \
        -w /src \
        ubuntu:22.04 \
        bash /src/scripts/build-rknpu-ko.sh
fi

# ─────────────────────────────────────────────────────────────────────────────
#  Everything below runs inside the Docker container (linux/amd64)
# ─────────────────────────────────────────────────────────────────────────────

CROSS="arm-rockchip830-linux-uclibcgnueabihf"
CACHE_DIR="/cache"
TOOLCHAIN_DIR="${CACHE_DIR}/toolchain"
KERNEL_CACHE="${CACHE_DIR}/kernel-src"
KERNEL_SRC="${KERNEL_CACHE}/sysdrv/source/kernel"
BUILD_DIR="/src/build-luckfox"

RKNPU_VERSION="0.9.8"
ROCKCHIP_BRANCH="develop-5.10"
ROCKCHIP_RAW="https://raw.githubusercontent.com/rockchip-linux/kernel/${ROCKCHIP_BRANCH}"
RKNPU_RAW="${ROCKCHIP_RAW}/drivers/rknpu"
JOBS="$(nproc)"

banner() {
    echo
    echo "──────────────────────────────────────────────────"
    printf '  %s\n' "$*"
    echo "──────────────────────────────────────────────────"
}

# ── Host build tools ──────────────────────────────────────────────────────────

banner "Installing host build tools"
apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    make git curl ca-certificates \
    build-essential flex bison libssl-dev bc python3 python-is-python3

# ── Toolchain ─────────────────────────────────────────────────────────────────

GCC="${TOOLCHAIN_DIR}/bin/${CROSS}-gcc"

if [[ ! -x "${GCC}" ]]; then
    banner "Downloading Luckfox toolchain → ${TOOLCHAIN_DIR}"
    mkdir -p "${TOOLCHAIN_DIR}"
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/LuckfoxTECH/luckfox-pico.git /tmp/luckfox-sdk
    cd /tmp/luckfox-sdk
    git sparse-checkout set "tools/linux/toolchain/${CROSS}"
    SDK_SRC="tools/linux/toolchain/${CROSS}"
    if [[ -d "${SDK_SRC}" ]]; then
        cp -a "${SDK_SRC}/." "${TOOLCHAIN_DIR}/"
    else
        echo "ERROR: toolchain not found in Luckfox SDK" >&2
        exit 1
    fi
    cd /
    rm -rf /tmp/luckfox-sdk
    chmod -R +x "${TOOLCHAIN_DIR}/bin"
else
    banner "Toolchain cached — skipping download"
fi

echo "Compiler: $("${GCC}" --version | head -1)"

# ── Luckfox BSP kernel source ─────────────────────────────────────────────────
#
# Blobless sparse clone: tree objects are downloaded at clone time; file blobs
# are fetched lazily when accessed.  The kernel source is checked out under
# sysdrv/source/kernel/ — git fetches the needed blobs during the checkout.
#
# vermagic of the target rknpu.ko: "5.10.160 mod_unload ARMv7 thumb2 p2v8"
# The built module must match this exactly — built from the same tree/config.

if [[ ! -f "${KERNEL_SRC}/Makefile" ]]; then
    banner "Cloning Luckfox BSP kernel 5.10.160 (blobless sparse, ~300 MB)"
    mkdir -p "${KERNEL_CACHE}"
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/LuckfoxTECH/luckfox-pico.git "${KERNEL_CACHE}"
    cd "${KERNEL_CACHE}"
    git sparse-checkout set "sysdrv/source/kernel"
    echo "  kernel source: ${KERNEL_SRC}"
else
    banner "Kernel source cached — skipping clone"
fi

# ── kernel modules_prepare ────────────────────────────────────────────────────
#
# Generates include/generated/autoconf.h, scripts/mod/modpost, and the other
# build infrastructure that a module build (M=...) depends on.
# This only needs to run once; the result is persisted in the cache.

PREPARED_STAMP="${KERNEL_CACHE}/.modules_prepared_luckfox_rv1106"

# ── Patch Luckfox BSP kernel headers (always applied, idempotent) ─────────────
#
# These patches fix BSP divergences from Rockchip upstream that cause compile
# errors when building rknpu 0.9.8 against the Luckfox kernel tree.

banner "Patching Luckfox BSP kernel headers for rknpu 0.9.8 compatibility"

# 1. version_compat_defs.h — present in Rockchip's develop-5.10 but not in the
#    Luckfox BSP fork.  rknpu_gem.c includes it.
curl -fsSL "${ROCKCHIP_RAW}/include/linux/version_compat_defs.h" \
    -o "${KERNEL_SRC}/include/linux/version_compat_defs.h"
echo "  added: include/linux/version_compat_defs.h"

# 2. rockchip_system_monitor.h — Luckfox BSP has a typo: MONITOR_TPYE_*
#    Rockchip upstream fixed the spelling to MONITOR_TYPE_*.
#    rknpu_devfreq.c uses the corrected spelling; patch the BSP header in-place.
MONITOR_HDR="${KERNEL_SRC}/include/soc/rockchip/rockchip_system_monitor.h"
if grep -q "MONITOR_TPYE_" "${MONITOR_HDR}" 2>/dev/null; then
    sed -i 's/MONITOR_TPYE_/MONITOR_TYPE_/g' "${MONITOR_HDR}"
    echo "  patched: include/soc/rockchip/rockchip_system_monitor.h (MONITOR_TPYE_ → MONITOR_TYPE_)"
else
    echo "  ok: include/soc/rockchip/rockchip_system_monitor.h (already correct)"
fi

if [[ ! -f "${PREPARED_STAMP}" ]]; then
    banner "Configuring kernel (luckfox_rv1106_linux_defconfig)"
    cd "${KERNEL_SRC}"
    make ARCH=arm \
         CROSS_COMPILE="${TOOLCHAIN_DIR}/bin/${CROSS}-" \
         luckfox_rv1106_linux_defconfig

    banner "Running modules_prepare"
    make ARCH=arm \
         CROSS_COMPILE="${TOOLCHAIN_DIR}/bin/${CROSS}-" \
         -j"${JOBS}" \
         modules_prepare

    touch "${PREPARED_STAMP}"
    echo "  modules_prepare complete"
else
    banner "modules_prepare cached — skipping"
fi

# ── Fetch rknpu 0.9.8 source ──────────────────────────────────────────────────
#
# All changed/new files from Rockchip develop-5.10:
#   rknpu_devfreq.c + include/rknpu_devfreq.h  — NEW (devfreq refactored out)
#   rknpu_iommu.c  — massively expanded (60 → 544 lines, domain switching)
#   rknpu_gem.c    — uses new iommu_switch_domain API + version_compat_defs.h
#   rknpu_drv.c    — devfreq code extracted; power_off workqueue added
#   rknpu_job.c    — uses new iommu domain per-job
#   rknpu_debugger.c, rknpu_reset.c, rknpu_fence.c, rknpu_mem.c, rknpu_mm.c
#   Makefile        — adds rknpu-$(CONFIG_PM_DEVFREQ) += rknpu_devfreq.o
#   All 11 headers replaced — rknpu_mem.h, rknpu_ioctl.h, rknpu_job.h etc.
#   all have API changes incompatible with 0.9.2; must update as a complete set.

banner "Fetching rknpu ${RKNPU_VERSION} sources from Rockchip (develop-5.10)"

cd "${KERNEL_SRC}/drivers/rknpu"

fetch_file() {
    local path="$1"
    local dir
    dir="$(dirname "${path}")"
    [[ "${dir}" != "." ]] && mkdir -p "${dir}"
    curl -fsSL "${RKNPU_RAW}/${path}" -o "${path}"
    echo "  fetched: ${path}"
}

# All source + header files — entire driver directory replaced atomically
for f in \
    rknpu_drv.c \
    rknpu_job.c \
    rknpu_gem.c \
    rknpu_iommu.c \
    rknpu_debugger.c \
    rknpu_reset.c \
    rknpu_fence.c \
    rknpu_mem.c \
    rknpu_mm.c \
    rknpu_devfreq.c \
    Makefile \
    include/rknpu_drv.h \
    include/rknpu_iommu.h \
    include/rknpu_devfreq.h \
    include/rknpu_debugger.h \
    include/rknpu_fence.h \
    include/rknpu_gem.h \
    include/rknpu_ioctl.h \
    include/rknpu_job.h \
    include/rknpu_mem.h \
    include/rknpu_mm.h \
    include/rknpu_reset.h; do
    fetch_file "${f}"
done

# ── Build rknpu.ko ────────────────────────────────────────────────────────────

banner "Building rknpu.ko ${RKNPU_VERSION}"
cd "${KERNEL_SRC}"
make ARCH=arm \
     CROSS_COMPILE="${TOOLCHAIN_DIR}/bin/${CROSS}-" \
     -j"${JOBS}" \
     M=drivers/rknpu \
     modules

# ── Strip and output ──────────────────────────────────────────────────────────

mkdir -p "${BUILD_DIR}"
cp -v drivers/rknpu/rknpu.ko "${BUILD_DIR}/rknpu.ko"

# Strip debug symbols — reduces size but keeps module loading symbols.
"${TOOLCHAIN_DIR}/bin/${CROSS}-strip" --strip-debug "${BUILD_DIR}/rknpu.ko"

# Verify the vermagic matches the running kernel.
VERMAGIC="$(strings "${BUILD_DIR}/rknpu.ko" | grep '^vermagic=' || true)"
echo "  vermagic: ${VERMAGIC}"
if [[ "${VERMAGIC}" != "vermagic=5.10.160 mod_unload ARMv7 thumb2 p2v8 " ]]; then
    echo "WARNING: vermagic does not match running kernel — module may not load." >&2
    echo "         Expected: vermagic=5.10.160 mod_unload ARMv7 thumb2 p2v8" >&2
fi

banner "Done"
echo "Module : build-luckfox/rknpu.ko"
echo
echo "Deploy :"
echo "  bash scripts/luckfox-install.sh --rknpu-only"
echo
