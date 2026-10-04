# Replaces Steam Audio's bundled FindMySOFA.cmake: libmysofa is already
# built for the engine (sp::mysofa, see cmake/Dependencies.cmake).
set(MySOFA_FOUND TRUE)
if(NOT TARGET MySOFA::MySOFA)
  add_library(MySOFA::MySOFA INTERFACE IMPORTED)
  target_link_libraries(MySOFA::MySOFA INTERFACE sp::mysofa)
endif()
