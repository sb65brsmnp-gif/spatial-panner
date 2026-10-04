# Applied to pffft after download (FetchContent PATCH_COMMAND). Idempotent.
# pffft registers its own tests and benchmarks with ctest unconditionally;
# we only build its library, so those would show up as "Not Run" in our
# test runs. Turn its add_test() calls into no-ops.
set(file "${PFFFT_SOURCE_DIR}/CMakeLists.txt")
file(READ "${file}" text)
if(NOT text MATCHES "pffft_no_test")
  string(REPLACE "add_test(" "pffft_no_test(" patched "${text}")
  # Keep enable_testing() so CMake still (re)generates this directory's
  # CTestTestfile, now without entries.
  string(REPLACE "enable_testing()" "enable_testing()\nfunction(pffft_no_test)\nendfunction()" patched "${patched}")
  file(WRITE "${file}" "${patched}")
  message(STATUS "Patched pffft test registration")
endif()
