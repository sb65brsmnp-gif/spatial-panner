cmake_minimum_required(VERSION 3.22)

# macOS builds are universal (Apple Silicon and Intel) unless told otherwise.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_ARCHITECTURES)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures" FORCE)
endif()

project(SpatialPanner VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SP_BUILD_TESTS "Build the engine unit tests" ON)
option(SP_BUILD_TOOLS "Build the command-line tools (renderer, signal generator)" ON)
option(SP_FETCH_HRTF  "Download the default HRTF (SADIE II KU100) at configure time" ON)
option(SP_BUILD_APP   "Build the standalone app (JUCE + the editor UI; needs npm)" ON)
option(SP_BUILD_PLUGIN "Build the plugin (AUv2 on macOS; needs SP_BUILD_APP for the editor UI)" ON)
option(SP_WITH_STEAM_AUDIO "Build Steam Audio from source for ray-traced reflections and occlusion" ON)

include(cmake/Dependencies.cmake)
if(SP_WITH_STEAM_AUDIO)
  include(cmake/SteamAudio.cmake)
endif()

if(SP_BUILD_TESTS)
  enable_testing()
endif()

add_subdirectory(engine)

if(SP_BUILD_TOOLS)
  add_subdirectory(tools/render)
  add_subdirectory(tools/gensignals)
  add_subdirectory(tools/scene)
endif()

if(SP_BUILD_APP)
  add_subdirectory(app)
  if(SP_BUILD_PLUGIN)
    add_subdirectory(plugin)
  endif()
endif()

if(SP_BUILD_TESTS)
  add_subdirectory(engine/tests)
endif()
