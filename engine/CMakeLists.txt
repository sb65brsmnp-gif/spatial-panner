# The engine: plain C++17, no JUCE. Depends on SAF (VBAP, Ambisonics, FFT),
# libmysofa (HRTF files) and nlohmann/json (scene files).

add_library(sp_engine STATIC
  src/Pose.cpp
  src/Materials.cpp
  src/Geometry.cpp
  src/SpeakerLayout.cpp
  src/SceneJson.cpp
  src/SceneAnalysis.cpp
  src/Renderer.cpp
  src/dsp/Fft.cpp
  src/dsp/Convolver.cpp
  src/dsp/AirAbsorption.cpp
  src/dsp/Hrtf.cpp
  src/dsp/Fdn.cpp
  src/dsp/IrReverb.cpp
  src/dsp/Resampler.cpp
  src/dsp/WavReader.cpp
  src/dsp/RoomAcoustics.cpp
  src/dsp/Panning.cpp
)
add_library(sp::engine ALIAS sp_engine)

target_include_directories(sp_engine
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(sp_engine
  PUBLIC  nlohmann_json::nlohmann_json
  PRIVATE saf sp::mysofa)

target_compile_options(sp_engine PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wno-unused-parameter>)

if(SP_WITH_STEAM_AUDIO)
  target_sources(sp_engine PRIVATE src/SteamAudioBackend.cpp)
  target_link_libraries(sp_engine PRIVATE sp::steamaudio)
  target_compile_definitions(sp_engine PUBLIC SP_HAVE_STEAM_AUDIO=1)
endif()

if(SP_DEFAULT_HRTF)
  target_compile_definitions(sp_engine PUBLIC SP_DEFAULT_HRTF="${SP_DEFAULT_HRTF}")
endif()
