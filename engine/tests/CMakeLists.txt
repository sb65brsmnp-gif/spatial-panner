add_executable(sp_tests
  test_math.cpp
  test_pose.cpp
  test_dsp.cpp
  test_renderer.cpp
  test_scene_json.cpp)
target_link_libraries(sp_tests PRIVATE sp::engine saf sp::mysofa Catch2::Catch2WithMain)
target_include_directories(sp_tests PRIVATE ${CMAKE_SOURCE_DIR}/engine/src)
target_compile_definitions(sp_tests PRIVATE SP_TEST_HRTF="${SP_DEFAULT_HRTF}")
include(Catch)
catch_discover_tests(sp_tests)
