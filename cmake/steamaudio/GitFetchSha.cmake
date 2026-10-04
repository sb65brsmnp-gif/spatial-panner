# Fetch one commit of a git repository by its hash, shallowly, into DEST.
#   cmake -DREPO=<url> -DSHA=<hash> -DDEST=<dir> -P GitFetchSha.cmake
# FetchContent's own GIT_SHALLOW only works when the hash is a branch or tag
# tip; this works for any commit the server still has (GitHub allows it).
find_package(Git REQUIRED QUIET)
if(EXISTS "${DEST}/.git")
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DEST}" rev-parse HEAD OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE r)
  if(r EQUAL 0 AND head STREQUAL SHA)
    return()
  endif()
endif()
file(MAKE_DIRECTORY "${DEST}")
foreach(step
    "init;-q"
    "remote;add;origin;${REPO}"
    "fetch;-q;--depth;1;origin;${SHA}"
    "-c;advice.detachedHead=false;checkout;-q;FETCH_HEAD")
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${DEST}" ${step} RESULT_VARIABLE r)
  if(NOT r EQUAL 0)
    message(FATAL_ERROR "git ${step} failed for ${REPO} @ ${SHA}")
  endif()
endforeach()
