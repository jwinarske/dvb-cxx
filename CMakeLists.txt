# SPDX-FileCopyrightText: 2026 Joel Winarske
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)
project(dvb-cxx LANGUAGES CXX)

# C++17 by default (hard floor, matching drm-cxx's CMake build which pins itself
# to C++17). Also builds clean at C++23: configure with -DCMAKE_CXX_STANDARD=23.
# drm::expected/drm::span aliasing is fixed by drm-cxx's generated config.h at
# configure time, so it stays consistent across both standards.
if(NOT DEFINED CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBAV  REQUIRED IMPORTED_TARGET libavcodec libavutil)
pkg_check_modules(LIBSWS REQUIRED IMPORTED_TARGET libswscale)
pkg_check_modules(LIBDRM REQUIRED IMPORTED_TARGET libdrm)
pkg_check_modules(LIBVA  IMPORTED_TARGET libva libva-drm)  # optional: VAAPI

# Build only the software (CPU) decoder; drop the VAAPI + V4L2 hardware backends
# (and the libva dependency) for a smaller CPU-only build.
option(TVTUNER_SOFTWARE_ONLY "Build only the software (CPU) decoder" OFF)

# drm-cxx is pinned as a git submodule (third_party/drm-cxx) and consumed via
# add_subdirectory; override DRM_CXX_DIR to build against a local working tree.
set(DRM_CXX_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/drm-cxx"
    CACHE PATH "Path to drm-cxx source tree")
if(NOT EXISTS "${DRM_CXX_DIR}/CMakeLists.txt")
  message(FATAL_ERROR
    "drm-cxx not found at ${DRM_CXX_DIR}. Run:\n"
    "  git submodule update --init --recursive\n"
    "or configure with -DDRM_CXX_DIR=/path/to/drm-cxx")
endif()

# Trim drm-cxx to what we need (no gstreamer/examples/tests/install).
set(DRM_CXX_GSTREAMER      OFF CACHE STRING "" FORCE)
set(DRM_CXX_BUILD_EXAMPLES OFF CACHE BOOL   "" FORCE)
set(DRM_CXX_BUILD_TESTS    OFF CACHE BOOL   "" FORCE)
set(DRM_CXX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(DRM_CXX_INSTALL        OFF CACHE BOOL   "" FORCE)

# drm-cxx's vendored deps (tcb-span, tl-expected, fmt, …) are meson wrap
# subprojects, git-ignored in the submodule; provision them once if absent.
if(NOT EXISTS "${DRM_CXX_DIR}/subprojects/tcb-span/include")
  find_program(MESON_EXECUTABLE meson)
  if(MESON_EXECUTABLE)
    message(STATUS "drm-cxx: downloading meson subprojects via ${MESON_EXECUTABLE}")
    execute_process(COMMAND ${MESON_EXECUTABLE} subprojects download
                    WORKING_DIRECTORY ${DRM_CXX_DIR})
  else()
    message(WARNING "meson not found; run 'meson subprojects download' in ${DRM_CXX_DIR}")
  endif()
endif()
add_subdirectory(${DRM_CXX_DIR} ${CMAKE_BINARY_DIR}/drm-cxx-build EXCLUDE_FROM_ALL)

set(DVB_KMS_SOURCES
  src/main.cpp
  src/kms_output.cpp
  src/kms_presenter.cpp
  src/decoder_source.cpp
  src/software_decoder_source.cpp
  src/dvb/frontend.cpp
  src/ts/ts_demux.cpp
  src/ts/ts_parse.cpp
)
set(DVB_KMS_DEFINES "")
set(DVB_KMS_LIBS
  drm-cxx::drm-cxx
  PkgConfig::LIBAV PkgConfig::LIBSWS PkgConfig::LIBDRM
  Threads::Threads)

if(NOT TVTUNER_SOFTWARE_ONLY)
  # V4L2 M2M backend — needs only drm-cxx's V4l2DecoderSource + videodev2.h.
  list(APPEND DVB_KMS_SOURCES src/v4l2_decoder_source_adapter.cpp)
  list(APPEND DVB_KMS_DEFINES TVTUNER_HAVE_V4L2)
  # VAAPI backend — needs libva (gracefully omitted when absent).
  if(LIBVA_FOUND)
    list(APPEND DVB_KMS_SOURCES
      src/vaapi_decoder.cpp src/vaapi_decoder_source.cpp)
    list(APPEND DVB_KMS_DEFINES TVTUNER_HAVE_VAAPI)
    list(APPEND DVB_KMS_LIBS PkgConfig::LIBVA)
  else()
    message(STATUS "dvb-cxx: libva not found — VAAPI backend disabled")
  endif()
endif()

add_executable(dvb-kms ${DVB_KMS_SOURCES})
target_include_directories(dvb-kms PRIVATE src)
target_link_libraries(dvb-kms PRIVATE ${DVB_KMS_LIBS})
target_compile_definitions(dvb-kms PRIVATE ${DVB_KMS_DEFINES})
target_compile_options(dvb-kms PRIVATE -Wall -Wextra)

# Unit tests for the pure TS/PSI parsing (no drm-cxx / DVB hardware needed).
enable_testing()
add_executable(test_ts_parse tests/test_ts_parse.cpp src/ts/ts_parse.cpp)
target_include_directories(test_ts_parse PRIVATE src)
target_compile_options(test_ts_parse PRIVATE -Wall -Wextra)
add_test(NAME ts_parse COMMAND test_ts_parse)
