# Applied to SAF after download (FetchContent PATCH_COMMAND).
#
# SAF's LAPACKE code path passes the CBLAS enum `CblasUpper` where LAPACKE
# expects the character 'U' (LAPACKE_?potrf_work / LAPACKE_?posv_work). With
# OpenBLAS + LAPACKE this makes every Cholesky factorisation fail ("On entry to
# CPOTRF parameter number 1 had an illegal value"), which silently breaks the
# diffuse-field matching of the binaural Ambisonics decoders. Apple Accelerate
# uses the CLAPACK interface and is not affected. Idempotent.
set(file "${SAF_SOURCE_DIR}/framework/modules/saf_utilities/saf_utility_veclib.c")
file(READ "${file}" text)
string(REPLACE "_work(CblasColMajor, CblasUpper," "_work(CblasColMajor, 'U'," patched "${text}")
if(NOT patched STREQUAL text)
  file(WRITE "${file}" "${patched}")
  message(STATUS "Patched SAF LAPACKE uplo arguments")
endif()
