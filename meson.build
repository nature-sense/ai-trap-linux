# ─────────────────────────────────────────────────────────────────────────────
#  meson.build — Insect Trap Pipeline
#
#  Source layout:
#    firmware/src/   all C++ source and header files
#    trap_config.toml  runtime config (not compiled)
#
#  Build targets (set with -Dtarget=):
#    libcamera     Pi 5 + IMX708 via libcamera         → yolo_libcamera
#    v4l2          Luckfox Pico Zero + IMX415 via V4L2  → yolo_v4l2
#    all           both (requires libcamera)
#
#  Native build:
#    meson setup build -Dtarget=libcamera && ninja -C build
#    meson setup build -Dtarget=v4l2      && ninja -C build
#
#  Cross-compile for Raspberry Pi 5 (aarch64) on Ubuntu x86:
#    sudo dpkg --add-architecture arm64
#    sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
#                         libcamera-dev:arm64
#    # Build NCNN + SQLite3 into /opt/pi5-sysroot (see cross/pi5-aarch64.ini)
#    meson setup build-pi5 \
#      --cross-file cross/pi5-aarch64.ini \
#      -Dtarget=libcamera \
#      -Dpi5_sysroot=/opt/pi5-sysroot
#    ninja -C build-pi5
#
#  Cross-compile for Luckfox Pico Zero (RV1106G3) on Ubuntu x86:
#    # Install arm-rockchip830-linux-uclibcgnueabihf toolchain first
#    # Build NCNN + SQLite3 into /opt/luckfox-sysroot (see cross/luckfox-rv1106.ini)
#    meson setup build-luckfox \
#      --cross-file cross/luckfox-rv1106.ini \
#      -Dtarget=v4l2 \
#      -Dluckfox_sysroot=/opt/luckfox-sysroot
#    ninja -C build-luckfox
# ─────────────────────────────────────────────────────────────────────────────

project('yolo-trap', 'cpp',
  default_options : ['cpp_std=c++17', 'optimization=2'])

# ── Cross-compilation: target detection ───────────────────────────────────────

is_arm_cross     = (meson.is_cross_build() and host_machine.cpu_family() == 'arm')
is_aarch64_cross = (meson.is_cross_build() and host_machine.cpu_family() == 'aarch64')

luckfox_sysroot = get_option('luckfox_sysroot')
pi5_sysroot     = get_option('pi5_sysroot')

# Resolve the active cross sysroot (empty string = not set)
_cross_sysroot = ''
if is_arm_cross and luckfox_sysroot != ''
  _cross_sysroot = luckfox_sysroot
  message('Luckfox (armv7) cross-build: sysroot = ' + luckfox_sysroot)
elif is_aarch64_cross and pi5_sysroot != ''
  _cross_sysroot = pi5_sysroot
  message('Pi 5 (aarch64) cross-build: sysroot = ' + pi5_sysroot)
endif

if _cross_sysroot != ''
  add_project_arguments(
    ['-isystem', _cross_sysroot + '/usr/include'],
    language : 'cpp')
  add_project_link_arguments(
    '-L' + _cross_sysroot + '/usr/lib',
    language : 'cpp')
endif

# ── CPU-specific compile flags ─────────────────────────────────────────────────
#
#  armv7  (Luckfox RV1106G3 / Cortex-A7):
#    -mfpu=neon-vfpv4  enables NEON intrinsics used by NCNN
#    -mfloat-abi=hard  matches the uClibc hard-float ABI
#
#  aarch64 (Raspberry Pi 5 / Cortex-A76):
#    +dotprod+fp16     enables NCNN ASIMD dot-product and FP16 kernels

if is_arm_cross
  _cpu_flags = [
    '-march=armv7-a',
    '-mtune=cortex-a7',
    '-mfpu=neon-vfpv4',
    '-mfloat-abi=hard',
    '-fomit-frame-pointer',
  ]
  add_project_arguments(_cpu_flags, language : 'cpp')
  add_project_arguments(_cpu_flags, language : 'c')
elif is_aarch64_cross
  _cpu_flags = [
    '-march=armv8.2-a+dotprod+fp16',
    '-mtune=cortex-a76',
  ]
  add_project_arguments(_cpu_flags, language : 'cpp')
  add_project_arguments(_cpu_flags, language : 'c')
endif

# ── Static C++ runtime for cross builds ───────────────────────────────────────
#
# Statically linking libstdc++ and libgcc_s produces a self-contained binary
# that runs on the target without matching the exact host toolchain version.
# glibc and libcamera remain dynamic (they are always present on the target OS).

if meson.is_cross_build()
  add_project_link_arguments(
    # Static C++ runtime — avoids toolchain version mismatch on target.
    '-static-libstdc++', '-static-libgcc',
    # libcamera.so's transitive deps (libpisp, libudev, libyaml, libdw, etc.)
    # are not in the staging sysroot but will be present on the Pi at runtime.
    # --allow-shlib-undefined suppresses linker errors for symbols that are
    # only needed by a shared library we link against (not by our code directly).
    '-Wl,--allow-shlib-undefined',
    language : 'cpp')
endif

# ── Dependencies ──────────────────────────────────────────────────────────────
#
# For cross-builds, NCNN and SQLite3 are located directly via find_library()
# from the staging sysroot — more reliable than pkg-config or cmake discovery
# in a cross environment.  libcamera is always discovered via pkg-config since
# it is installed to the system multiarch path by apt.

cxx = meson.get_compiler('cpp')

thread_dep    = dependency('threads',   required : true)
omp_dep       = dependency('openmp',    required : false)
# libcamera pkg-config name changed across versions:
#   Ubuntu 22.04 (0.0.4)   → 'camera'
#   Raspberry Pi OS / newer → 'libcamera'
# Try the modern name first, fall back to the old one.
libcamera_dep = dependency('libcamera', required : false)
if not libcamera_dep.found()
  libcamera_dep = dependency('camera', required : false)
endif

if _cross_sysroot != ''
  _lib      = _cross_sysroot + '/usr/lib'
  _lib_arch = _cross_sysroot + '/usr/lib/aarch64-linux-gnu'

  ncnn_dep    = cxx.find_library('ncnn',    dirs : [_lib], required : true)
  sqlite3_dep = cxx.find_library('sqlite3', dirs : [_lib], required : true)

  # libcamera: if pkg-config already found it (e.g. via the sysroot
  # pkg-config wrapper) keep that result.  Only fall back to find_library
  # if pkg-config came up empty.
  if is_aarch64_cross and not libcamera_dep.found()
    libcamera_dep = cxx.find_library('camera',
                      dirs     : [_lib_arch, _lib],
                      required : false)
  endif

  # RKNN NPU mini-runtime — Luckfox v4l2 target only.
  # librknnmrt.so and rknn_api.h are fetched from the Luckfox SDK by
  # build-luckfox-mac.sh and installed to $SYSROOT/usr/lib and usr/include.
  # RV1106 uses the mini-runtime (librknnmrt), not the full librknnrt.
  if is_arm_cross
    rknn_dep = cxx.find_library('rknnmrt', dirs : [_lib], required : true)
  endif
else
  sqlite3_dep = dependency('sqlite3', required : true)

  ncnn_dep = dependency('ncnn', required : false)
  if not ncnn_dep.found()
    cmake = import('cmake')
    ncnn_opts = cmake.subproject_options()
    ncnn_opts.add_cmake_defines({
      'NCNN_BUILD_EXAMPLES'  : 'OFF',
      'NCNN_BUILD_TOOLS'     : 'OFF',
      'NCNN_BUILD_BENCHMARK' : 'OFF',
      'NCNN_VULKAN'          : 'OFF',
    })
    ncnn_proj = cmake.subproject('ncnn', options : ncnn_opts)
    # NCNN's build tree exposes headers under src/ directly, but the
    # codebase uses #include "ncnn/mat.h".  The generated meson.build
    # adds '.' (the subproject root) to the include path, so a
    # ncnn/ -> src/ symlink there makes both styles work.
    run_command('ln', '-sfn',
      meson.project_source_root() / 'subprojects/ncnn/src',
      meson.project_source_root() / 'subprojects/ncnn/ncnn',
      check : true)
    ncnn_dep  = ncnn_proj.dependency('ncnn')
  endif
endif

# Bluetooth headers — needed for BLE GATT server (Pi 5 and Luckfox).
# We use only macros/inline functions from these headers; no libbluetooth.so or
# libbluetooth.a is linked, so there is zero runtime dependency on BlueZ packages.
# Headers come from the sysroot (see pi5_sysroot / luckfox_sysroot options).
cc = meson.get_compiler('cpp')

# ── Option: select build target ───────────────────────────────────────────────

target_opt = get_option('target')   # 'libcamera' | 'v4l2' | 'all'

# ── Shared sources (compiled into both targets) ───────────────────────────────

shared_sources = files(
  'firmware/src/pipeline/decoder.cpp',
  'firmware/src/pipeline/tracker.cpp',
  'firmware/src/pipeline/persistence.cpp',
  'firmware/src/pipeline/crop_saver.cpp',
  'firmware/src/pipeline/exif_writer.cpp',
  'firmware/src/pipeline/sync_manager.cpp',
  'firmware/src/server/mjpeg_streamer.cpp',
  'firmware/src/server/sse_server.cpp',
  'firmware/src/server/http_server.cpp',
  'firmware/src/common/stb_image_write_impl.cpp',
  'firmware/src/common/wifi_manager.cpp',
  'firmware/src/common/ble_gatt_server.cpp',
  'firmware/src/common/epaper_display.cpp',
)

shared_deps = [ncnn_dep, sqlite3_dep, thread_dep, omp_dep]

inc = include_directories([
                            'firmware/src/common',
                            'firmware/src/io',
                            'firmware/src/pipeline',
                            'firmware/src/server'
                          ])

# ── Pi 5 / libcamera target ───────────────────────────────────────────────────

if target_opt == 'libcamera' or target_opt == 'all'
  if not libcamera_dep.found()
    error('target=libcamera requires libcamera. ' +
          'On the Luckfox cross-build host use -Dtarget=v4l2 instead.')
  endif
  executable('yolo_libcamera',
    sources : shared_sources + files(
      'firmware/src/io/libcamera_capture.cpp',
      'firmware/main_libcamera.cpp',
    ),
    include_directories : inc,
    dependencies        : shared_deps + [libcamera_dep],
    cpp_args            : ['-DWIFI_BACKEND_NM'],
    # Bake /opt/ai-trap/lib into the ELF RPATH so the binary finds the
    # bundled libcamera shared libraries without LD_LIBRARY_PATH.
    install_rpath       : '/opt/ai-trap/lib',
  )
endif

# ── Luckfox Pico Zero / IMX415 / V4L2 target ─────────────────────────────────
#
# RKNN NPU inference is the only supported backend for the Luckfox target.
# ncnn is retained as a library dependency because ncnn::Mat is used as the
# shared tensor transport type throughout the capture and decoder pipeline.

if target_opt == 'v4l2' or target_opt == 'all'
  executable('yolo_v4l2',
    sources : shared_sources + files(
      'firmware/src/io/v4l2_capture.cpp',
      'firmware/src/io/rknn_infer.cpp',
      'firmware/main_v4l2.cpp',
    ),
    include_directories : inc,
    dependencies        : shared_deps + [rknn_dep],
    cpp_args            : ['-DWIFI_BACKEND_HOSTAPD'],
  )
endif
