#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  build-luckfox-mac.sh
#
#  Cross-compiles yolo_v4l2 for the Luckfox Pico Zero (RV1106G3, Cortex-A7)
#  on an Apple Silicon (or Intel) Mac using Docker.
#
#  Inference runs on the RV1106 NPU via the Rockchip RKNN runtime.
#  ncnn is retained as a library (ncnn::Mat is the shared tensor type) but
#  is not used for inference.
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
#  All deps (toolchain, SQLite3, NCNN, librknnmrt) are cached in:
#    ~/.cache/ai-trap/luckfox/
#  Subsequent builds skip the download/compile phase (~25 min first time).
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

SQLITE_VERSION="3450100"
SQLITE_YEAR="2024"
NCNN_VERSION="20240410"
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

# ── NCNN ──────────────────────────────────────────────────────────────────────
#
# ncnn::Mat is the shared tensor transport type used throughout the capture and
# decoder pipeline.  Inference itself runs on the RKNN NPU, not ncnn::Net.

if [[ ! -f "${SYSROOT}/usr/lib/libncnn.a" ]]; then
    banner "Cross-compiling NCNN ${NCNN_VERSION}"
    git clone --depth 1 --branch "${NCNN_VERSION}" \
        https://github.com/Tencent/ncnn.git /tmp/ncnn-src
    cmake -S /tmp/ncnn-src -B /tmp/ncnn-build \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=arm \
        -DCMAKE_C_COMPILER="${GCC}" \
        -DCMAKE_CXX_COMPILER="${GXX}" \
        -DCMAKE_C_FLAGS="${ARM_FLAGS}" \
        -DCMAKE_CXX_FLAGS="${ARM_FLAGS}" \
        -DCMAKE_INSTALL_PREFIX="${SYSROOT}/usr" \
        -DNCNN_TARGET_ARCH=arm \
        -DNCNN_SHARED_LIB=OFF \
        -DNCNN_BUILD_TESTS=OFF \
        -DNCNN_BUILD_EXAMPLES=OFF \
        -DNCNN_BUILD_TOOLS=OFF \
        -DNCNN_BUILD_BENCHMARK=OFF \
        -DNCNN_VULKAN=OFF \
        -DCMAKE_BUILD_TYPE=Release
    cmake --build /tmp/ncnn-build -j"${JOBS}"
    cmake --install /tmp/ncnn-build
    rm -rf /tmp/ncnn-src /tmp/ncnn-build
else
    banner "NCNN cached — skipping build"
fi

# ── RKNN runtime (librknnmrt.so + rknn_api.h) ────────────────────────────────
#
# RV1106 uses the RKNN *mini* runtime (librknnmrt.so), not the full librknnrt.so
# (which is for higher-end chips like RK3588).
#
# Both files ship in the Luckfox SDK and are cached in the sysroot after the
# first fetch.
#
# SDK paths tried in order:
#   lib:    media/iva/iva/librockiva/rockiva-rv1106-Linux/lib/librknnmrt.so
#           project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-glibc-rockchip/usr/lib/librknnmrt.so
#   header: project/app/rk_smart_door/smart_door/common/face/algo/rknn_api.h
#           project/app/rk_smart_door/smart_door/common/face/algo_dual_ir/include/rknn_api.h

if [[ ! -f "${SYSROOT}/usr/lib/librknnmrt.so" ]]; then
    banner "Fetching RKNN mini-runtime from Luckfox SDK"
    git clone --depth 1 --filter=blob:none --sparse \
        https://github.com/LuckfoxTECH/luckfox-pico.git /tmp/luckfox-sdk-npu
    cd /tmp/luckfox-sdk-npu
    git sparse-checkout set \
        "media/iva/iva/librockiva/rockiva-rv1106-Linux" \
        "project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-glibc-rockchip/usr/lib" \
        "project/app/rk_smart_door/smart_door/common/face/algo"

    RKNN_LIB=""
    for p in \
        "media/iva/iva/librockiva/rockiva-rv1106-Linux/lib/librknnmrt.so" \
        "project/cfg/BoardConfig_IPC/overlay/overlay-luckfox-glibc-rockchip/usr/lib/librknnmrt.so"; do
        [[ -f "${p}" ]] && RKNN_LIB="${p}" && break
    done

    RKNN_HDR=""
    for p in \
        "project/app/rk_smart_door/smart_door/common/face/algo/rknn_api.h" \
        "project/app/rk_smart_door/smart_door/common/face/algo_dual_ir/include/rknn_api.h"; do
        [[ -f "${p}" ]] && RKNN_HDR="${p}" && break
    done

    if [[ -z "${RKNN_LIB}" || -z "${RKNN_HDR}" ]]; then
        echo "ERROR: could not find librknnmrt.so or rknn_api.h in Luckfox SDK." >&2
        echo "       SDK tree (rknn-related files):" >&2
        git ls-tree -r --name-only HEAD | grep -i rknn | head -30 >&2 || true
        exit 1
    fi

    mkdir -p "${SYSROOT}/usr/lib" "${SYSROOT}/usr/include"
    cp "${RKNN_LIB}" "${SYSROOT}/usr/lib/librknnmrt.so"
    cp "${RKNN_HDR}" "${SYSROOT}/usr/include/rknn_api.h"

    cd /
    rm -rf /tmp/luckfox-sdk-npu
    echo "librknnmrt.so → ${SYSROOT}/usr/lib/"
    echo "rknn_api.h    → ${SYSROOT}/usr/include/"
else
    banner "RKNN mini-runtime cached — skipping fetch"
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
cpp_args = ['-isystem', '${SYSROOT}/usr/include']
c_args   = ['-isystem', '${SYSROOT}/usr/include']
EOF

# ── Configure + build ─────────────────────────────────────────────────────────

banner "Configuring (meson)"
rm -rf "${BUILD_DIR}"
meson setup "${BUILD_DIR}" /src \
    --cross-file "${CROSS_FILE}" \
    -Dtarget=v4l2 \
    -Dluckfox_sysroot="${SYSROOT}" \
    --buildtype=release

banner "Building (ninja)"
ninja -C "${BUILD_DIR}"

# ── Strip ─────────────────────────────────────────────────────────────────────

banner "Stripping binary"
"${BIN}-strip" "${BUILD_DIR}/yolo_v4l2"

banner "Done"
echo "Binary : build-luckfox/yolo_v4l2"
echo
echo "Deploy :"
echo "  bash scripts/luckfox-install.sh"
echo
echo "Model conversion (if model.rknn not yet generated):"
echo "  bash firmware/models/scripts/convert-rknn.sh \\"
echo "      firmware/models/yolo11n-320/model.onnx \\"
echo "      firmware/models/yolo11n-320/model.rknn \\"
echo "      firmware/models/calibration"
