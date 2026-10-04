cmake_minimum_required(VERSION 3.22)

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
option(SP_WITH_STEAM_AUDIO "Build Steam Audio from source for ray-traced reflections and occlusion" ON)

include(cmake/Dependencies.cmake)
if(SP_WITH_STEAM_AUDIO)
  include(cmake/SteamAudio.cmake)
endif()

add_subdirectory(engine)

if(SP_BUILD_TOOLS)
  add_subdirectory(tools/render)
  add_subdirectory(tools/gensignals)
endif()

if(SP_BUILD_TESTS)
  enable_testing()
  add_subdirectory(engine/tests)
endif()
