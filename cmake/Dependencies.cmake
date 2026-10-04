# Third-party dependencies, fetched at configure time and pinned to commits.
#
#   libmysofa      BSD-3   SOFA (HRTF) file loading
#   SAF            ISC     VBAP, Ambisonics (AllRAD, binaural decoders), FFT
#   nlohmann/json  MIT     scene files
#   dr_libs        MIT-0   WAV read/write in the tools
#   Catch2         BSL-1.0 unit tests
#   JUCE 9         AGPLv3 / JUCE licence (Starter tier for personal use)  the app
#
# SAF needs a BLAS/LAPACK provider: Apple Accelerate on macOS, OpenBLAS +
# LAPACKE elsewhere (apt: libopenblas-dev liblapacke-dev).

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- libmysofa
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_Declare(libmysofa
  GIT_REPOSITORY https://github.com/hoene/libmysofa.git
  GIT_TAG        648eed03472e6720a1ea45d1a1f86c4efb569ff9
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(libmysofa)
# libmysofa's targets do not export their include directories for consumers.
add_library(sp::mysofa INTERFACE IMPORTED)
target_link_libraries(sp::mysofa INTERFACE mysofa-static)
target_include_directories(sp::mysofa INTERFACE
  ${libmysofa_SOURCE_DIR}/src/hrtf
  ${libmysofa_BINARY_DIR}/src)

# ---------------------------------------------------------------------- SAF
if(APPLE)
  set(SAF_PERFORMANCE_LIB "SAF_USE_APPLE_ACCELERATE_LP64" CACHE STRING "")
else()
  set(SAF_PERFORMANCE_LIB "SAF_USE_OPEN_BLAS_AND_LAPACKE" CACHE STRING "")
  if(NOT DEFINED OPENBLAS_HEADER_PATH)
    find_path(OPENBLAS_HEADER_PATH cblas.h
      PATHS /usr/include/x86_64-linux-gnu/openblas-pthread
            /usr/include/aarch64-linux-gnu/openblas-pthread
            /usr/include/openblas
            /usr/include/x86_64-linux-gnu
            /usr/include)
  endif()
  if(NOT OPENBLAS_HEADER_PATH)
    message(FATAL_ERROR "cblas.h not found. Install OpenBLAS (apt: libopenblas-dev liblapacke-dev) "
                        "or pass -DOPENBLAS_HEADER_PATH=...")
  endif()
endif()
set(SAF_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAF_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SAF_USE_FAST_MATH_FLAG OFF CACHE BOOL "" FORCE)
FetchContent_Declare(saf
  GIT_REPOSITORY https://github.com/leomccormack/Spatial_Audio_Framework.git
  GIT_TAG        18fd5aba46e20787b51f28f7197a68506c965c07
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  framework
  PATCH_COMMAND  ${CMAKE_COMMAND} -DSAF_SOURCE_DIR=<SOURCE_DIR> -P ${CMAKE_CURRENT_LIST_DIR}/PatchSaf.cmake
  UPDATE_DISCONNECTED TRUE)
FetchContent_MakeAvailable(saf)

# ------------------------------------------------------------ nlohmann/json
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.12.0
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(nlohmann_json)

# ------------------------------------------------------------------ dr_libs
if(SP_BUILD_TOOLS)
  FetchContent_Declare(dr_libs
    GIT_REPOSITORY https://github.com/mackron/dr_libs.git
    GIT_TAG        dfe8377631000664666519fdb83da193fd8037f4)
  FetchContent_Populate(dr_libs)
  add_library(sp::dr_libs INTERFACE IMPORTED)
  target_include_directories(sp::dr_libs INTERFACE ${dr_libs_SOURCE_DIR})
endif()

# ------------------------------------------------------------------- Catch2
if(SP_BUILD_TESTS)
  FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.8.1
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(Catch2)
  list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# ---------------------------------------------------------------- HRTF data
# SADIE II subject D1 (Neumann KU100), 8802 directions, 48 kHz, 256 taps.
# Apache 2.0. Fetched from the Steam Audio repository, which redistributes it.
set(SP_HRTF_DIR ${CMAKE_BINARY_DIR}/data/hrtf)
set(SP_DEFAULT_HRTF ${SP_HRTF_DIR}/sadie_d1.sofa CACHE FILEPATH "Default HRTF SOFA file")
if(SP_FETCH_HRTF AND NOT EXISTS ${SP_DEFAULT_HRTF})
  message(STATUS "Downloading default HRTF (SADIE II KU100, 35 MB)...")
  file(DOWNLOAD
    https://raw.githubusercontent.com/ValveSoftware/steam-audio/480dd64f513cc8a6437e7d5b9eb0d3f1d30c2fac/core/data/hrtf/sadie_d1.sofa
    ${SP_DEFAULT_HRTF}
    EXPECTED_HASH SHA256=e6c72a84dd947b5ef75438ab96a9c2a32ed10f033472b9c4c11a49aff00a8a31
    SHOW_PROGRESS
    STATUS hrtf_status)
  list(GET hrtf_status 0 hrtf_code)
  if(NOT hrtf_code EQUAL 0)
    file(REMOVE ${SP_DEFAULT_HRTF})
    message(WARNING "HRTF download failed: ${hrtf_status}. Pass -DSP_DEFAULT_HRTF=/path/to/file.sofa")
  endif()
endif()

# --------------------------------------------------------------------- JUCE
if(SP_BUILD_APP)
  FetchContent_Declare(juce
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.3  # be29c81492b6151c8ea8d14c840e1311963b3a83
    GIT_SHALLOW    TRUE)
  FetchContent_MakeAvailable(juce)
endif()
