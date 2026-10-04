# Replaces Steam Audio's bundled FindFlatBuffers.cmake (which expects a
# pre-built flatc under deps/). We build flatc ourselves from the same
# pinned FlatBuffers commit, see cmake/SteamAudio.cmake.
set(FlatBuffers_INCLUDE_DIR ${flatbuffers_SOURCE_DIR}/include)
set(FlatBuffers_EXECUTABLE $<TARGET_FILE:flatc>)
set(FlatBuffers_VERSION 1.12.0)
set(FlatBuffers_FOUND TRUE)
if(NOT TARGET FlatBuffers::FlatBuffers)
  add_library(FlatBuffers INTERFACE)
  target_include_directories(FlatBuffers INTERFACE ${FlatBuffers_INCLUDE_DIR})
  add_library(FlatBuffers::FlatBuffers ALIAS FlatBuffers)
endif()
