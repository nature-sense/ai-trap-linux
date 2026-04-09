#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build-luckfox-mac.sh
#
#  Cross-compiles yolo_v4l2 for the Luckfox Pico Zero (RV1106G3, Cortex-A7)
#  on an Apple Silicon (or Intel) Mac using Docker.
#
#  Inference runs on the RV1106 NPU via the Rockchip RKNN runtime.
#
#  Requirements:
#    Docker Desktop — https://www.docker.com/products/docker-desktop/
#
#  Usage:
#    bash scripts/build-luckfox-mac.sh
#
#  Output:
#    build-luckfox/yolo_v4l2
#
#  All deps (toolchain, SQLite3, librknnmrt) are cached in:
#    ~/.cache/ai-trap/luckfox/
#  Subsequent builds skip the download/compile phase.
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# ── macOS: re-exec inside Docker ─────────────────────────────────────────────
#
# The Luckfox toolchain is an x86_64 Linux ELF binary.  On macOS we run it
# inside an ubuntu:22.04 container (linux/amd64); Docker Desktop handles the
# Rosetta 2 translation on Apple Silicon automatically.

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
        bash /src/scripts/build-luckfox-mac.sh
fi

# ─────────────────────────────────────────────────────────────────────────────
#  Everything below runs inside the Docker container (linux/amd64)
# ─────────────────────────────────────────────────────────────────────────────

CROSS="arm-rockchip830-linux-uclibcgnueabihf"
CACHE_DIR="/cache"
TOOLCHAIN_DIR="${CACHE_DIR}/toolchain"
SYSROOT="${CACHE_DIR}/sysroot"
BUILD_DIR="/src/build-luckfox"

SQLITE_VERSION="3510300"
SQLITE_YEAR="2026"
ARM_FLAGS="-march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2"
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
    cmake ninja-build meson pkg-config make \
    curl ca-certificates git \
    gcc \
    libbluetooth-dev

# ── Toolchain ─────────────────────────────────────────────────────────────────

GCC="${TOOLCHAIN_DIR}/bin/${CROSS}-gcc"
GXX="${TOOLCHAIN_DIR}/bin/${CROSS}-g++"

if [[ ! -x "${GCC}" ]]; then
    banner "Downloading Luckfox toolchain → ${TOOLCHAIN_DIR}"
    mkdir -p "${TOOLCHAIN_DIR}"
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/LuckfoxTECH/luckfox-pico.git /tmp/luckfox-sdk
    cd /tmp/luckfox-sdk
    git sparse-checkout set "tools/linux/toolchain/${CROSS}"
    SDK_SRC="tools/linux/toolchain/${CROSS}"
    if [[ -d "${SDK_SRC}" ]]; then
        rm -rf "${TOOLCHAIN_DIR}"
        mkdir -p "${TOOLCHAIN_DIR}"
        cp -a "${SDK_SRC}/." "${TOOLCHAIN_DIR}/"
    else
        echo "ERROR: toolchain not found in Luckfox SDK" >&2
        ls tools/linux/toolchain/ >&2 || true
        exit 1
    fi
    cd /
    rm -rf /tmp/luckfox-sdk
    chmod -R +x "${TOOLCHAIN_DIR}/bin"
else
    banner "Toolchain cached — skipping download"
fi

echo "Compiler: $("${GCC}" --version | head -1)"

# ── BlueZ headers ─────────────────────────────────────────────────────────────

banner "Copying BlueZ headers into sysroot"
mkdir -p "${SYSROOT}/usr/include/bluetooth"
for f in bluetooth.h hci.h hci_lib.h l2cap.h; do
    cp -v "/usr/include/bluetooth/${f}" "${SYSROOT}/usr/include/bluetooth/"
done

# ── SQLite3 ───────────────────────────────────────────────────────────────────

if [[ ! -f "${SYSROOT}/usr/lib/libsqlite3.a" ]]; then
    banner "Cross-compiling SQLite3 ${SQLITE_VERSION}"
    mkdir -p /tmp/sqlite-build
    curl -fsSL \
        "https://www.sqlite.org/${SQLITE_YEAR}/sqlite-autoconf-${SQLITE_VERSION}.tar.gz" \
        | tar xz -C /tmp/sqlite-build
    cd "/tmp/sqlite-build/sqlite-autoconf-${SQLITE_VERSION}"
    ./configure \
        --host="${CROSS}" \
        --prefix=/usr \
        --enable-static \
        --disable-shared \
        --disable-dependency-tracking \
        CC="${GCC}" \
        AR="${TOOLCHAIN_DIR}/bin/${CROSS}-ar" \
        RANLIB="${TOOLCHAIN_DIR}/bin/${CROSS}-ranlib" \
        CFLAGS="${ARM_FLAGS}"
    make -j"${JOBS}"
    make DESTDIR="${SYSROOT}" install
    cd /
    rm -rf /tmp/sqlite-build
else
    banner "SQLite3 cached — skipping build"
fi

# ── libjpeg-turbo ─────────────────────────────────────────────────────────────
#
# Used by softwareJpegLoop in rkmpi_capture.cpp for fast NEON-accelerated JPEG
# encoding of the 640×480 MJPEG stream.  Replaces stb_image_write which has no
# SIMD and costs ~100 ms per frame on the Cortex-A7.
#
# Built as a static library (libjpeg.a + libturbojpeg.a) so there is no runtime
# dependency — the binary is fully self-contained.
#
# libjpeg-turbo uses CMake.  We invoke it with the cross toolchain via
# CMAKE_TOOLCHAIN_FILE-equivalent flags and install into the sysroot.

LIBJPEG_TURBO_VERSION="3.1.0"

if [[ ! -f "${SYSROOT}/usr/lib/libjpeg.a" ]]; then
    banner "Cross-compiling libjpeg-turbo ${LIBJPEG_TURBO_VERSION}"
    mkdir -p /tmp/jpeg-build
    curl -fsSL \
        "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${LIBJPEG_TURBO_VERSION}/libjpeg-turbo-${LIBJPEG_TURBO_VERSION}.tar.gz" \
        | tar xz -C /tmp/jpeg-build
    mkdir -p /tmp/jpeg-build/build
    cmake \
        -S "/tmp/jpeg-build/libjpeg-turbo-${LIBJPEG_TURBO_VERSION}" \
        -B /tmp/jpeg-build/build \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=arm \
        -DCMAKE_C_COMPILER="${TOOLCHAIN_DIR}/bin/${CROSS}-gcc" \
        -DCMAKE_CXX_COMPILER="${TOOLCHAIN_DIR}/bin/${CROSS}-g++" \
        -DCMAKE_AR="${TOOLCHAIN_DIR}/bin/${CROSS}-ar" \
        -DCMAKE_RANLIB="${TOOLCHAIN_DIR}/bin/${CROSS}-ranlib" \
        -DCMAKE_C_FLAGS="${ARM_FLAGS}" \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DENABLE_SHARED=OFF \
        -DENABLE_STATIC=ON \
        -DWITH_JPEG8=1 \
        -DWITH_SIMD=ON \
        -DREQUIRE_SIMD=OFF
    ninja -C /tmp/jpeg-build/build -j"${JOBS}"
    DESTDIR="${SYSROOT}" ninja -C /tmp/jpeg-build/build install
    rm -rf /tmp/jpeg-build
    echo "libjpeg-turbo → ${SYSROOT}/usr/lib/libjpeg.a"
else
    banner "libjpeg-turbo cached — skipping build"
fi

# ── RKNN runtime (librknnmrt.so + rknn_api.h) ────────────────────────────────
#
# RV1106 uses the RKNN *mini* runtime (librknnmrt.so), not the full librknnrt.so
# (which is for higher-end chips like RK3588).
#
# VERSION PINNING — this must match the toolkit2 version used in
# Dockerfile.rknn-toolkit2.  Mismatched versions cause rknn_inputs_set to
# fail at runtime.  Both files are fetched from the matching GitHub release:
#   https://github.com/airockchip/rknn-toolkit2/releases/tag/v2.3.2
#
# armhf-uclibc: correct variant for Luckfox Pico Zero (Cortex-A7, uClibc).
# Driver compatibility: librknnmrt.so v2.x requires rknpu driver > 0.8.0.
# The Luckfox BSP ships driver 0.9.2 which satisfies this — no BSP update needed.

RKNN_VERSION="v2.3.2"
RKNN_BASE="https://raw.githubusercontent.com/airockchip/rknn-toolkit2/${RKNN_VERSION}/rknpu2/runtime/Linux/librknn_api"

if [[ ! -f "${SYSROOT}/usr/lib/librknnmrt.so" ]]; then
    banner "Fetching RKNN mini-runtime ${RKNN_VERSION} from GitHub"
    mkdir -p "${SYSROOT}/usr/lib" "${SYSROOT}/usr/include"

    curl -fL "${RKNN_BASE}/armhf-uclibc/librknnmrt.so" \
         -o "${SYSROOT}/usr/lib/librknnmrt.so" || {
        echo "ERROR: failed to download librknnmrt.so" >&2; exit 1
    }
    curl -fL "${RKNN_BASE}/include/rknn_api.h" \
         -o "${SYSROOT}/usr/include/rknn_api.h" || {
        echo "ERROR: failed to download rknn_api.h" >&2; exit 1
    }

    LIB_VER="$(strings "${SYSROOT}/usr/lib/librknnmrt.so" | grep 'librknnmrt version:' || true)"
    echo "librknnmrt.so → ${SYSROOT}/usr/lib/  (${LIB_VER})"
    echo "rknn_api.h    → ${SYSROOT}/usr/include/"
else
    LIB_VER="$(strings "${SYSROOT}/usr/lib/librknnmrt.so" | grep 'librknnmrt version:' || true)"
    banner "RKNN mini-runtime cached — skipping fetch  (${LIB_VER})"
fi

# ── RKMPI / Rockit headers + library ─────────────────────────────────────────
#
# librockit.so (lib32) and RKMPI headers are fetched from the Luckfox SDK:
#   media/rockit/rockit/mpi/sdk/include/   → SYSROOT/usr/include/rkmpi/
#   media/rockit/rockit/lib/lib32/librockit.so → SYSROOT/usr/lib/
#
# librockit.so is already present on the device at /usr/lib/librockit.so —
# we only need it in the sysroot for linking.
#
# The sparse checkout is done into /tmp/luckfox-sdk-rockit and removed after.

ROCKIT_MARKER="${SYSROOT}/usr/include/rkmpi/rk_mpi_vi.h"

if [[ ! -f "${ROCKIT_MARKER}" ]]; then
    banner "Fetching RKMPI headers + librockit.so from Luckfox SDK"
    mkdir -p "${SYSROOT}/usr/include/rkmpi" "${SYSROOT}/usr/lib"

    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/LuckfoxTECH/luckfox-pico.git /tmp/luckfox-sdk-rockit
    cd /tmp/luckfox-sdk-rockit
    git sparse-checkout set \
        "media/rockit/rockit/mpi/sdk/include" \
        "media/rockit/rockit/lib/lib32"

    ROCKIT_INC="media/rockit/rockit/mpi/sdk/include"
    ROCKIT_LIB="media/rockit/rockit/lib/lib32"

    if [[ -d "${ROCKIT_INC}" ]]; then
        cp -v "${ROCKIT_INC}/"*.h "${SYSROOT}/usr/include/rkmpi/"
        echo "RKMPI headers → ${SYSROOT}/usr/include/rkmpi/"
    else
        echo "ERROR: RKMPI headers not found in Luckfox SDK" >&2
        ls media/rockit/ >&2 || true
        exit 1
    fi

    if [[ -f "${ROCKIT_LIB}/librockit.so" ]]; then
        cp -v "${ROCKIT_LIB}/librockit.so" "${SYSROOT}/usr/lib/"
        echo "librockit.so → ${SYSROOT}/usr/lib/"
    else
        echo "ERROR: librockit.so not found in Luckfox SDK" >&2
        ls "${ROCKIT_LIB}/" >&2 || true
        exit 1
    fi

    cd /
    rm -rf /tmp/luckfox-sdk-rockit
else
    banner "RKMPI headers + librockit.so cached — skipping fetch"
fi

# ── Meson cross file ─────────────────────────────────────────────────────────

banner "Generating meson cross file"
BIN="${TOOLCHAIN_DIR}/bin/${CROSS}"
CROSS_FILE="/tmp/luckfox-rv1106-mac.ini"
cat > "${CROSS_FILE}" <<EOF
[binaries]
c          = '${BIN}-gcc'
cpp        = '${BIN}-g++'
ar         = '${BIN}-ar'
strip      = '${BIN}-strip'
objcopy    = '${BIN}-objcopy'
pkg-config = 'false'

[host_machine]
system     = 'linux'
cpu_family = 'arm'
cpu        = 'cortex-a7'
endian     = 'little'

[properties]
sys_root          = '${SYSROOT}'
pkg_config_libdir = '${SYSROOT}/usr/lib/pkgconfig:${SYSROOT}/usr/share/pkgconfig'
cmake_prefix_path = ['${SYSROOT}/usr']

[built-in options]
cpp_args = ['-isystem', '${SYSROOT}/usr/include', '-isystem', '${SYSROOT}/usr/include/rkmpi']
c_args   = ['-isystem', '${SYSROOT}/usr/include', '-isystem', '${SYSROOT}/usr/include/rkmpi']
EOF

# ── Configure + build ─────────────────────────────────────────────────────────

banner "Configuring (meson)"
rm -rf "${BUILD_DIR}"
meson setup "${BUILD_DIR}" /src \
    --cross-file "${CROSS_FILE}" \
    -Dtarget=all \
    -Dluckfox_sysroot="${SYSROOT}" \
    --buildtype=release

banner "Building (ninja)"
ninja -C "${BUILD_DIR}"

# ── Strip ─────────────────────────────────────────────────────────────────────

banner "Stripping binaries"
"${BIN}-strip" "${BUILD_DIR}/yolo_v4l2"
[[ -f "${BUILD_DIR}/yolo_rkmpi" ]] && "${BIN}-strip" "${BUILD_DIR}/yolo_rkmpi"

banner "Done"
echo "Binaries:"
echo "  build-luckfox/yolo_v4l2   (stable V4L2 baseline)"
echo "  build-luckfox/yolo_rkmpi  (RKMPI+VPSS prototype, if librockit.so found)"
echo
echo "Deploy :"
echo "  bash scripts/luckfox-install.sh"
echo
echo "Model conversion (if model.rknn not yet generated):"
echo "  bash firmware/models/scripts/convert-rknn.sh \\"
echo "      firmware/models/yolo11n-320/model.onnx \\"
echo "      firmware/models/yolo11n-320/model.rknn \\"
echo "      firmware/models/calibration"
