# ─────────────────────────────────────────────────────────────────────────────
#  meson.build — Insect Trap Pipeline
#
#  Source layout:
#    src/              all C++ source and header files
#    trap_config.toml  runtime config (not compiled)
#
#  Build targets (set with -Dtarget=):
#    libcamera     Pi 5 + IMX708 via libcamera         → yolo_libcamera
#    v4l2          Luckfox Pico Zero + IMX415 via V4L2  → yolo_v4l2
#    all           both (requires libcamera)
#
#  Native examples:
#    meson setup build -Dtarget=libcamera && ninja -C build
#    meson setup build -Dtarget=v4l2      && ninja -C build
#
#  Cross-compile for Luckfox Pico Zero (RV1106G3) on Ubuntu x86:
#    source luckfox-pico/env_install_toolchain.sh
#    meson setup build-luckfox \
#      --cross-file cross/luckfox-rv1106.ini \
#      -Dtarget=v4l2
#    ninja -C build-luckfox
#
#  Optional: override the sysroot that contains cross-compiled NCNN/SQLite3:
#    meson setup build-luckfox \
#      --cross-file cross/luckfox-rv1106.ini \
#      -Dtarget=v4l2 \
#      -Dluckfox_sysroot=/path/to/sysroot
# ─────────────────────────────────────────────────────────────────────────────

project('yolo-trap', 'cpp',
  default_options : ['cpp_std=c++17', 'optimization=2'])

# ── Cross-compilation: sysroot override ───────────────────────────────────────
#
# When building with --cross-file cross/luckfox-rv1106.ini the sysroot is
# already set in [properties], but the user can override it here if the
# cross-compiled deps live somewhere else.

luckfox_sysroot = get_option('luckfox_sysroot')

is_arm_cross = (meson.is_cross_build() and
                host_machine.cpu_family() == 'arm')

if is_arm_cross and luckfox_sysroot != ''
  # Redirect pkg-config to the user-supplied sysroot at configure time.
  # (meson propagates this to the pkg-config invocations automatically when
  # sys_root / pkg_config_libdir are set in the cross file; this block lets
  # the user override those values without editing the cross file.)
  message('Luckfox build: using sysroot ' + luckfox_sysroot)
  add_project_arguments(
    '-isystem' + luckfox_sysroot + '/usr/include',
    language : 'cpp')
  add_project_link_arguments(
    '-L' + luckfox_sysroot + '/usr/lib',
    language : 'cpp')
endif

# ── ARM Cortex-A7 / NEON compile flags ────────────────────────────────────────
#
# Applied only when cross-compiling for the RV1106G3 target.
# -mfpu=neon-vfpv4   enables NEON intrinsics used by NCNN's ARM kernels
# -mfloat-abi=hard   matches the uClibC hard-float ABI of the toolchain

arm_flags = []
if is_arm_cross
  arm_flags = [
    '-march=armv7-a',
    '-mtune=cortex-a7',
    '-mfpu=neon-vfpv4',
    '-mfloat-abi=hard',
    '-fomit-frame-pointer',  # saves a register on the single-core A7
  ]
  add_project_arguments(arm_flags, language : 'cpp')
  add_project_arguments(arm_flags, language : 'c')
endif

# ── Dependencies ──────────────────────────────────────────────────────────────

ncnn_dep    = dependency('ncnn',    required : true)
sqlite3_dep = dependency('sqlite3', required : true)
thread_dep  = dependency('threads', required : true)
omp_dep     = dependency('openmp',  required : false)
libcamera_dep = dependency('libcamera', required : false)

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
  'firmware/src/common/ble_ctrl.cpp',
)

shared_deps = [ncnn_dep, sqlite3_dep, thread_dep, omp_dep,]

inc = include_directories([
                            'firmware/src/common',
                            'firmware/src/io',
                            'firmware/src/pipeline',
                            'firmware/src/server'
                          ])

# ── Pi 5 / libcamera target ───────────────────────────────────────────────────

if (target_opt == 'libcamera' or target_opt == 'all') and libcamera_dep.found()
  executable('yolo_libcamera',
    sources : shared_sources + files(
      'firmware/src/io/libcamera_capture.cpp',
      'firmware/main_libcamera.cpp',
    ),
    include_directories : inc,
    dependencies        : shared_deps + [libcamera_dep],
    cpp_args            : ['-DWIFI_BACKEND_NM'],
  )
endif

# ── Luckfox Pico Zero / IMX415 / V4L2 target ─────────────────────────────────

if target_opt == 'v4l2' or target_opt == 'all'
  executable('yolo_v4l2',
    sources : shared_sources + files(
      'firmware/src/io/v4l2_capture.cpp',
      'firmware/main_v4l2.cpp',
    ),
    include_directories : inc,
    dependencies        : shared_deps,
    cpp_args            : ['-DWIFI_BACKEND_HOSTAPD'],
  )
endif
