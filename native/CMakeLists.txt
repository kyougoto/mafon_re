# =============================================================================
#  mafon native libraries - Android arm64-v8a / x86_64 / armeabi-v7a
#
#  Builds, from the pristine upstream sources in ../src:
#    * libmpg123.a  - mpg123 1.32.10 libmpg123 core, static, -fPIC, generic C
#    * libsonic.so  - waywardgeek/sonic-ndk (org.vinuxproject.sonic JNI)
#    * libmpg.so    - the MP3Decoder JNI wrapper target (nobleworks.libmpg);
#                     uses jni/mpg_jni.c when present, otherwise a placeholder
#
#  Android shared libraries are linked with -Wl,-z,max-page-size=16384 by
#  default (16 KB page-size compatible for Android 15+; see MAFON_MAX_PAGE_SIZE).
#
#  Usage (normally driven by ./build.ps1):
#    cmake -G Ninja -S . -B build/arm64-v8a \
#          -DCMAKE_TOOLCHAIN_FILE=<NDK>/build/cmake/android.toolchain.cmake \
#          -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 \
#          -DCMAKE_BUILD_TYPE=Release \
#          -DCMAKE_INSTALL_PREFIX=<this dir>/out/arm64-v8a
#    cmake --build build/arm64-v8a
#    cmake --install build/arm64-v8a
# =============================================================================
cmake_minimum_required(VERSION 3.20)

project(mafon-native LANGUAGES C)

# ---------------------------------------------------------------------------
# Paths / options
# ---------------------------------------------------------------------------
get_filename_component(_mafon_root "${CMAKE_CURRENT_SOURCE_DIR}" ABSOLUTE)
get_filename_component(_mafon_work "${_mafon_root}/.." ABSOLUTE)

set(MAFON_SRC_ROOT "${_mafon_work}/src" CACHE PATH
    "Directory containing the pristine mpg123/ and sonic/ source trees")
set(MPG123_SRC "${MAFON_SRC_ROOT}/mpg123" CACHE PATH "mpg123 1.32.10 source tree")
set(SONIC_SRC  "${MAFON_SRC_ROOT}/sonic"  CACHE PATH "waywardgeek/sonic-ndk source tree")

if(ANDROID_ABI)
  set(MAFON_ABI "${ANDROID_ABI}")
else()
  set(MAFON_ABI "host")
endif()

set(MAFON_OUT "${_mafon_root}/out/${MAFON_ABI}" CACHE PATH
    "Staging directory for the built libraries and headers")

option(MAFON_BUILD_MPG123 "Build the static libmpg123.a" ON)
option(MAFON_BUILD_SONIC  "Build libsonic.so" ON)
# The libmpg.so target always exists; build.ps1 sets this OFF when jni/mpg_jni.c
# is absent and the caller did not explicitly ask for the placeholder build.
option(MAFON_BUILD_MPG "Build the libmpg.so JNI target" ON)

# 16 KB page size compatibility: Android 15+/16 devices with 16 KB memory pages
# require ELF LOAD segments aligned to 16 KB. NDK r26b's lld still defaults to
# 4 KB (p_align 0x1000), so request 16 KB explicitly for every shared library.
# Set to 0 to use the linker default.
set(MAFON_MAX_PAGE_SIZE 16384 CACHE STRING
    "Android: ELF max-page-size passed to the linker (0 = linker default)")

# Applies the Android page-size alignment to a SHARED/executable target.
function(mafon_android_link_options target)
  if(ANDROID AND MAFON_MAX_PAGE_SIZE GREATER 0)
    target_link_options(${target} PRIVATE
      "LINKER:-z,max-page-size=${MAFON_MAX_PAGE_SIZE}")
  endif()
endfunction()

# All linkable artifacts land directly in out/<abi>/ ; headers in out/<abi>/include
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${MAFON_OUT}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${MAFON_OUT}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${MAFON_OUT}")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
# Everything is linked into a .so, so the static mpg123 archive must be PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

message(STATUS "mafon-native: ABI=${MAFON_ABI} platform=${ANDROID_PLATFORM} out=${MAFON_OUT}")

# ---------------------------------------------------------------------------
# JNI include dirs (jni.h comes from the NDK sysroot via the toolchain file)
# ---------------------------------------------------------------------------
if(NOT ANDROID)
  message(WARNING "This project is meant to be configured with the Android NDK toolchain file.")
endif()

# ===========================================================================
#  libmpg123.a  (mpg123 1.32.10, libmpg123 core, generic C decoders)
# ===========================================================================
if(MAFON_BUILD_MPG123)
  if(NOT EXISTS "${MPG123_SRC}/src/libmpg123/libmpg123.c")
    message(FATAL_ERROR "mpg123 sources not found under ${MPG123_SRC}. "
                        "Extract mpg123-1.32.10 into ${MAFON_SRC_ROOT}/mpg123 first.")
  endif()

  # --- generated config.h (Android specific, see cmake/config-mpg123.h.in) ---
  set(MPG123_SIZEOF_OFF_T 8)
  configure_file(
    "${_mafon_root}/cmake/config-mpg123.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/mpg123-config/config.h"
    @ONLY)

  set(_mpg123_dir "${MPG123_SRC}/src/libmpg123")
  set(_compat_dir "${MPG123_SRC}/src/compat")

  add_library(mpg123static STATIC
    "${_mpg123_dir}/parse.c"
    "${_mpg123_dir}/frame.c"
    "${_mpg123_dir}/format.c"
    "${_mpg123_dir}/dct64.c"
    "${_mpg123_dir}/equalizer.c"
    "${_mpg123_dir}/id3.c"
    "${_mpg123_dir}/optimize.c"
    "${_mpg123_dir}/readers.c"
    "${_mpg123_dir}/tabinit.c"
    "${_mpg123_dir}/libmpg123.c"
    "${_mpg123_dir}/index.c"
    "${_mpg123_dir}/icy.c"
    "${_mpg123_dir}/icy2utf8.c"
    "${_mpg123_dir}/layer1.c"
    "${_mpg123_dir}/layer2.c"
    "${_mpg123_dir}/layer3.c"
    "${_mpg123_dir}/ntom.c"
    "${_mpg123_dir}/synth_8bit.c"
    "${_mpg123_dir}/synth.c"
    "${_mpg123_dir}/synth_s32.c"
    "${_mpg123_dir}/synth_real.c"
    "${_mpg123_dir}/stringbuf.c"
    "${_mpg123_dir}/feature.c"
    "${_mpg123_dir}/lfs_wrap.c"
    "${_compat_dir}/compat.c"
    "${_compat_dir}/compat_str.c"
  )
  set_target_properties(mpg123static PROPERTIES
    OUTPUT_NAME "mpg123"
    C_VISIBILITY_PRESET default)

  target_include_directories(mpg123static PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/mpg123-config"   # generated config.h
    "${MPG123_SRC}/src"
    "${MPG123_SRC}/src/include"
    "${MPG123_SRC}/src/libmpg123")

  # Plain C decoding: pick the generic (portable) synth/dct implementation.
  # REAL_IS_FLOAT matches IEEE_FLOAT in the generated config.h.
  target_compile_definitions(mpg123static PRIVATE
    OPT_GENERIC
    REAL_IS_FLOAT
    $<$<EQUAL:${CMAKE_SIZEOF_VOID_P},4>:_FILE_OFFSET_BITS=64>)

  if(ANDROID)
    target_link_libraries(mpg123static PUBLIC m)
  endif()

  # Keep every symbol (the JNI wrapper resolves them at link time; the archive
  # is not stripped here).
  install(TARGETS mpg123static
    ARCHIVE DESTINATION "."
    LIBRARY DESTINATION "."
    RUNTIME DESTINATION ".")
endif()

# ===========================================================================
#  libsonic.so  (waywardgeek/sonic-ndk, org.vinuxproject.sonic JNI wrapper)
# ===========================================================================
if(MAFON_BUILD_SONIC)
  set(_sonic_dir "${SONIC_SRC}/jni")
  if(NOT EXISTS "${_sonic_dir}/sonic.c")
    message(FATAL_ERROR "sonic sources not found under ${_sonic_dir}. "
                        "Clone waywardgeek/sonic-ndk into ${MAFON_SRC_ROOT}/sonic first.")
  endif()

  add_library(sonic SHARED
    "${_sonic_dir}/sonic.c"
    "${_sonic_dir}/sonicjni.c")
  set_target_properties(sonic PROPERTIES
    OUTPUT_NAME "sonic"
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(sonic PRIVATE "${_sonic_dir}")

  if(ANDROID)
    # Android.mk of sonic-ndk links -llog; sonicjni.c includes <android/log.h>.
    target_link_libraries(sonic PRIVATE log m)
  endif()
  mafon_android_link_options(sonic)

  install(TARGETS sonic
    LIBRARY DESTINATION "."
    RUNTIME DESTINATION ".")
endif()

# ===========================================================================
#  libmpg.so  (MP3Decoder JNI wrapper, package nobleworks.libmpg)
#
#  The JNI C source lives at native/jni/mpg_jni.c.
#  * present  -> it is compiled and linked against libmpg123.a
#  * absent   -> a placeholder is compiled so the target always exists, but it
#                is written to build/.../placeholder and NOT staged into out/.
# ===========================================================================
if(MAFON_BUILD_MPG)
  set(MAFON_JNI_SOURCE "${_mafon_root}/jni/mpg_jni.c" CACHE FILEPATH
      "MP3Decoder JNI wrapper source")

  if(EXISTS "${MAFON_JNI_SOURCE}")
    set(_mpg_jni_src "${MAFON_JNI_SOURCE}")
    set(_mpg_jni_real TRUE)
    message(STATUS "mafon-native: libmpg.so uses REAL JNI source ${MAFON_JNI_SOURCE}")
  else()
    set(_mpg_jni_src "${_mafon_root}/jni/mpg_jni_placeholder.c")
    set(_mpg_jni_real FALSE)
    message(WARNING
      "mafon-native: ${MAFON_JNI_SOURCE} not found - building libmpg.so from a "
      "placeholder. The placeholder is NOT installed into out/. Drop the real "
      "mpg_jni.c there and re-run build.ps1.")
  endif()

  add_library(mpg SHARED "${_mpg_jni_src}")
  set_target_properties(mpg PROPERTIES
    OUTPUT_NAME "mpg"
    POSITION_INDEPENDENT_CODE ON)
  target_include_directories(mpg PRIVATE
    "${_mafon_root}/jni"
    "${MPG123_SRC}/src/include")

  # Android MediaExtractor/MediaCodec fallback decoder: makes m4a/AAC, flac,
  # ogg/vorbis, opus, wav, ... playable through the same MP3Decoder JNI API.
  if(_mpg_jni_real)
    target_sources(mpg PRIVATE "${_mafon_root}/jni/media_decoder.c")
  endif()

  if(TARGET mpg123static)
    target_link_libraries(mpg PRIVATE mpg123static)
  endif()
  if(ANDROID)
    target_link_libraries(mpg PRIVATE log m mediandk)
  endif()
  mafon_android_link_options(mpg)

  if(_mpg_jni_real)
    # Finally a real JNI wrapper: stage it next to the other libraries.
    install(TARGETS mpg
      LIBRARY DESTINATION "."
      RUNTIME DESTINATION ".")
  else()
    # Keep the placeholder out of the staging dir; it is only a build check.
    set_target_properties(mpg PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/placeholder")
  endif()
endif()

# ===========================================================================
#  Staged public headers -> out/<abi>/include
# ===========================================================================
set(_mafon_headers
  "${MPG123_SRC}/src/include/mpg123.h"
  "${MPG123_SRC}/src/include/fmt123.h"
  "${SONIC_SRC}/jni/sonic.h")
foreach(_h IN LISTS _mafon_headers)
  if(NOT EXISTS "${_h}")
    message(FATAL_ERROR "Required header missing: ${_h}")
  endif()
endforeach()
install(FILES ${_mafon_headers} DESTINATION "include")

# Convenience: `cmake --build <dir> --target stage` builds then installs.
set(_mafon_stage_deps "")
foreach(_t mpg123static sonic)
  if(TARGET ${_t})
    list(APPEND _mafon_stage_deps ${_t})
  endif()
endforeach()
if(TARGET mpg AND _mpg_jni_real)
  list(APPEND _mafon_stage_deps mpg)
endif()
add_custom_target(stage
  COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_BINARY_DIR}"
  DEPENDS ${_mafon_stage_deps}
  COMMENT "Staging libraries and headers into ${CMAKE_INSTALL_PREFIX}"
  USES_TERMINAL)

