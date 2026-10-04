# Steam Audio (Valve, Apache 2.0), built from source as a static library and
# linked into the engine. Used for ray-traced reflections / convolution reverb
# against arbitrary geometry, and for occlusion / transmission.
#
# Steam Audio's own build expects pre-built dependencies under core/deps/
# (its build/get_dependencies.py produces them). Instead we build the three
# required ones here and hand them over through replacement Find modules in
# cmake/steamaudio/:
#
#   FlatBuffers 1.12  (flatc + headers, same commit Steam Audio pins)
#   pffft             (FFT; the commit Steam Audio pins)
#   libmysofa         (already built for the engine)
#
# Everything optional in Steam Audio (IPP, Embree, Radeon Rays, TrueAudio
# Next, tests, samples) is off.

include(FetchContent)

# FlatBuffers 1.12 and pffft declare cmake_minimum_required 2.8; CMake 4
# (GitHub's macOS runners) refuses that without this. Steam Audio itself is 3.17.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
# We call FetchContent_Populate + add_subdirectory(EXCLUDE_FROM_ALL) so only
# the libraries phonon needs get built; CMake 3.30 deprecates that form.
if(POLICY CMP0169)
  cmake_policy(SET CMP0169 OLD)
endif()

# ----------------------------------------------------------- FlatBuffers 1.12
set(FLATBUFFERS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATLIB  OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
set(FLATBUFFERS_BUILD_FLATC    ON  CACHE BOOL "" FORCE)
set(FLATBUFFERS_INSTALL        OFF CACHE BOOL "" FORCE)
FetchContent_Declare(flatbuffers
  GIT_REPOSITORY https://github.com/google/flatbuffers.git
  GIT_TAG        6df40a2471737b27271bdd9b900ab5f3aec746c7   # v1.12.0
  GIT_SHALLOW    TRUE)
FetchContent_GetProperties(flatbuffers)
if(NOT flatbuffers_POPULATED)
  FetchContent_Populate(flatbuffers)
  add_subdirectory(${flatbuffers_SOURCE_DIR} ${flatbuffers_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
# FlatBuffers 1.12 builds with -Werror; compilers newer than 2020 warn.
target_compile_options(flatc PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-error -w>)

# -------------------------------------------------------------------- pffft
set(PFFFT_USE_TYPE_DOUBLE   OFF CACHE BOOL "" FORCE)
set(PFFFT_USE_FFTPACK       OFF CACHE BOOL "" FORCE)
set(PFFFT_USE_BENCH_GREEN   OFF CACHE BOOL "" FORCE)
set(PFFFT_USE_BENCH_KISS    OFF CACHE BOOL "" FORCE)
set(PFFFT_USE_BENCH_POCKET  OFF CACHE BOOL "" FORCE)
set(INSTALL_PFFFT           OFF CACHE BOOL "" FORCE)
# Pinned to commits that are not branch tips, so fetched by hash (see GitFetchSha.cmake).
set(SP_PFFFT_SHA e0bf595c98ded55cc457a371c1b29c8cab552628)
FetchContent_Declare(pffft
  DOWNLOAD_COMMAND ${CMAKE_COMMAND} -DREPO=https://github.com/marton78/pffft.git -DSHA=${SP_PFFFT_SHA}
                   -DDEST=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/steamaudio/GitFetchSha.cmake
  PATCH_COMMAND  ${CMAKE_COMMAND} -DPFFFT_SOURCE_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/PatchPffft.cmake
  UPDATE_DISCONNECTED TRUE)
FetchContent_GetProperties(pffft)
if(NOT pffft_POPULATED)
  FetchContent_Populate(pffft)
  # EXCLUDE_FROM_ALL: only the PFFFT library (pulled in by phonon) gets built,
  # not pffft's examples and benchmarks.
  add_subdirectory(${pffft_SOURCE_DIR} ${pffft_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

# -------------------------------------------------------------- Steam Audio
set(STEAMAUDIO_BUILD_TESTS       OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_BUILD_BENCHMARKS  OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_BUILD_SAMPLES     OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_BUILD_ITESTS      OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_BUILD_DOCS        OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_ENABLE_IPP        OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_ENABLE_MKL        OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_ENABLE_EMBREE     OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_ENABLE_RADEONRAYS OFF CACHE BOOL "" FORCE)
set(STEAMAUDIO_ENABLE_TRUEAUDIONEXT OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

# Our Find modules must win over Steam Audio's (it APPENDs its own dir).
list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/steamaudio)

# master after v4.8.1: the tag itself overwrites CMAKE_MODULE_PATH (so our
# Find modules would be lost) and lacks a few upstream fixes.
set(SP_STEAMAUDIO_SHA 480dd64f513cc8a6437e7d5b9eb0d3f1d30c2fac)
FetchContent_Declare(steamaudio
  DOWNLOAD_COMMAND ${CMAKE_COMMAND} -DREPO=https://github.com/ValveSoftware/steam-audio.git -DSHA=${SP_STEAMAUDIO_SHA}
                   -DDEST=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/steamaudio/GitFetchSha.cmake
  SOURCE_SUBDIR  core
  PATCH_COMMAND  ${CMAKE_COMMAND} -DSTEAMAUDIO_SOURCE_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/PatchSteamAudio.cmake
  UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(steamaudio)

# The .fbs schemas are compiled with our flatc at build time.
add_dependencies(fbschemas flatc)

add_library(sp::steamaudio INTERFACE IMPORTED)
target_link_libraries(sp::steamaudio INTERFACE phonon)
target_include_directories(sp::steamaudio INTERFACE
  ${steamaudio_SOURCE_DIR}/core/src/core        # phonon.h
  ${steamaudio_BINARY_DIR}/src/core)            # phonon_version.h (generated)

# The repository also carries the default HRTF; no separate download needed.
set(SP_STEAMAUDIO_HRTF ${steamaudio_SOURCE_DIR}/core/data/hrtf/sadie_d1.sofa)
