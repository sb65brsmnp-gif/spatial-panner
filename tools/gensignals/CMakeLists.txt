add_executable(sp-gensignals main.cpp)
target_link_libraries(sp-gensignals PRIVATE sp::engine sp::dr_libs)
target_include_directories(sp-gensignals PRIVATE ${CMAKE_SOURCE_DIR}/tools/common)
