add_executable(sp-render main.cpp)
target_link_libraries(sp-render PRIVATE sp::engine sp::dr_libs saf)
target_include_directories(sp-render PRIVATE ${CMAKE_SOURCE_DIR}/tools/common)
