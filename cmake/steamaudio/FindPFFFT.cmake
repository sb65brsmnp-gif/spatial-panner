# Replaces Steam Audio's bundled FindPFFFT.cmake: pffft is built in this
# tree (target PFFFT), see cmake/SteamAudio.cmake.
set(PFFFT_INCLUDE_DIR ${pffft_SOURCE_DIR})
set(PFFFT_FOUND TRUE)
if(NOT TARGET PFFFT::PFFFT)
  add_library(PFFFT::PFFFT INTERFACE IMPORTED)
  target_link_libraries(PFFFT::PFFFT INTERFACE PFFFT)
  target_include_directories(PFFFT::PFFFT INTERFACE ${PFFFT_INCLUDE_DIR})
endif()
