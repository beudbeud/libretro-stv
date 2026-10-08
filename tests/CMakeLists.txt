# Headless tests: built with -DSTV_BUILD_TESTS=ON, never part of the core.
#
#   dspjit_fuzz_test  SCSP DSP JIT vs interpreter on random programs (links the
#                     core library; skips itself on platforms without the JIT)
#   stv_headless      runs the libretro core with no frontend: hashes of the
#                     video/audio output plus ms/frame (see the file header)
#   run_dspjit_enc.sh checks the AArch64 encoders against GNU as (host script)
#
# mdfn_core exports its include directories, definitions and dependencies.

add_executable(dspjit_fuzz_test dspjit_fuzz_test.cpp)
target_link_libraries(dspjit_fuzz_test PRIVATE mdfn_core)

add_executable(stv_headless stv_headless.c)
target_include_directories(stv_headless PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(stv_headless PRIVATE ${CMAKE_DL_LIBS})
