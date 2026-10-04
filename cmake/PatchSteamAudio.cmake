# Applied to Steam Audio after download (FetchContent PATCH_COMMAND).
# Idempotent. Only build-system changes; no source changes.
#
# 1. core/CMakeLists.txt forces a universal (x86_64;arm64) build and a 10.13
#    deployment target on macOS. Architecture and deployment target are
#    decided by the top-level project (the plugin build will set them for
#    the whole tree), so those two lines go.
# 2. src/core/CMakeLists.txt has a post-build step that copies the built
#    library into ../fmod/ next to the repository. Disabled.
# 3. install(FILES ${CMAKE_HOME_DIRECTORY}/THIRDPARTY.md ...) assumes Steam
#    Audio is the top-level project; pointed at its own source dir.
# 4. On Linux it compiles with -fabi-version=6 (an old __m256 mangling
#    workaround). GCC 13's <future> does not compile under that ABI version,
#    and the default ABI mangles AVX types fine. Removed.
# 5. One source fix: ArrayMath::multiplyAccumulate (complex) takes an
#    "unaligned" SSE branch when its inputs are not 16-byte aligned but still
#    does an aligned load of the accumulator there. The convolution effect's
#    accumulator rows are 8 bytes apart from alignment on every other
#    channel (an odd number of complex bins per row), so without IPP (which
#    the shipped binaries use) this crashes. Switched to an unaligned load.

function(sp_patch file)
  file(READ "${file}" text)
  set(patched "${text}")
  math(EXPR n "${ARGC} - 1")
  set(i 1)
  while(i LESS ${ARGC})
    math(EXPR j "${i} + 1")
    string(REPLACE "${ARGV${i}}" "${ARGV${j}}" patched "${patched}")
    math(EXPR i "${i} + 2")
  endwhile()
  if(NOT patched STREQUAL text)
    file(WRITE "${file}" "${patched}")
    message(STATUS "Patched ${file}")
  endif()
endfunction()

sp_patch("${STEAMAUDIO_SOURCE_DIR}/core/CMakeLists.txt"
  "    set(CMAKE_OSX_ARCHITECTURES \"x86_64;arm64\")\n" ""
  "    set(CMAKE_OSX_DEPLOYMENT_TARGET \"10.13\")\n" ""
  "install(FILES \${CMAKE_HOME_DIRECTORY}/THIRDPARTY.md" "install(FILES \${CMAKE_CURRENT_SOURCE_DIR}/THIRDPARTY.md"
  "        add_compile_options(-fabi-version=6)\n" "")

sp_patch("${STEAMAUDIO_SOURCE_DIR}/core/src/core/array_math.cpp"
  "            y = float4::add(y, float4::load(&outData[i]));\n\n            float4::storeu(&outData[i], y);"
  "            y = float4::add(y, float4::loadu(&outData[i]));\n\n            float4::storeu(&outData[i], y);")

sp_patch("${STEAMAUDIO_SOURCE_DIR}/core/src/core/CMakeLists.txt"
  "if (NOT FMOD_LIB_DIR STREQUAL \"\")" "if (FALSE) # fmod copy step disabled by spatial-panner")
