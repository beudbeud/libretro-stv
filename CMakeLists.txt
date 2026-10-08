cmake_minimum_required(VERSION 3.16)

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --abbrev=0
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_TAG
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE GIT_RESULT
    )
    if(GIT_RESULT EQUAL 0)
        string(REGEX REPLACE "^v" "" GIT_VERSION "${GIT_TAG}")
    else()
        set(GIT_VERSION "0.0.0")
    endif()
else()
    set(GIT_VERSION "0.0.0")
endif()

# project(VERSION ...) only accepts numeric major[.minor[.patch[.tweak]]].
# Tags like "10.1-patron-1-alpha-8" carry extra labels, so extract the
# leading numeric portion for project() and keep the full tag for display.
if(GIT_VERSION MATCHES "^([0-9]+(\\.[0-9]+)*)")
    set(GIT_VERSION_NUMERIC "${CMAKE_MATCH_1}")
else()
    set(GIT_VERSION_NUMERIC "0.0.0")
endif()

project(mednafen_stv VERSION ${GIT_VERSION_NUMERIC} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -fsigned-char -fwrapv -fexceptions
    -fno-fast-math -fno-unsafe-math-optimizations
    -fstrict-aliasing
)

# GCC-only flags — not supported by Clang/AppleClang
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    add_compile_options(
        -fno-aggressive-loop-optimizations
        -fno-ipa-icf
        -fno-printf-return-value
    )
endif()

# Profile-guided optimization (aarch64 GCC only; ~6% less CPU on the Pi 5).
# The profile in pgo/ is used automatically. To regenerate it after code
# changes: configure with -DSTV_PGO_GENERATE=/tmp/pgo, then on the target run
# tests/stv_headless with that core over in-game savestates (-l, 3000 frames
# each of decathlt and vfkids) and their attract mode (3600 frames), and
# replace pgo/ with the target's /tmp/pgo/*.gcda. Stale or mismatching
# profiles only lose the optimization for the affected functions.
set(STV_PGO_GENERATE "" CACHE PATH "Build an instrumented core writing its profile to this directory")
option(STV_PGO_USE "Use the profile in pgo/ when present" ON)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    if(STV_PGO_GENERATE)
        add_compile_options(-fprofile-generate=${STV_PGO_GENERATE} -fprofile-update=atomic
                            -fprofile-prefix-path=${CMAKE_BINARY_DIR})
        add_link_options(-fprofile-generate=${STV_PGO_GENERATE})
    elseif(STV_PGO_USE AND EXISTS ${CMAKE_SOURCE_DIR}/pgo)
        add_compile_options(-fprofile-use=${CMAKE_SOURCE_DIR}/pgo -fprofile-partial-training
                            -fprofile-prefix-path=${CMAKE_BINARY_DIR}
                            -Wno-missing-profile -Wno-coverage-mismatch)
    endif()
endif()

if(NOT DEFINED LIBRETRO_CORE_VERSION)
    set(LIBRETRO_CORE_VERSION "${GIT_VERSION}")
endif()

add_compile_definitions(
    _LARGEFILE_SOURCE _LARGEFILE64_SOURCE _FILE_OFFSET_BITS=64
    HAVE_CONFIG_H
    LIBRETRO_CORE_VERSION="${LIBRETRO_CORE_VERSION}"
    $<$<CONFIG:Release>:_FORTIFY_SOURCE=1>
)

configure_file(${CMAKE_SOURCE_DIR}/libretro/config_libretro.h
               ${CMAKE_BINARY_DIR}/config.h COPYONLY)

add_subdirectory(mednafen)
add_subdirectory(libretro)

option(STV_BUILD_TESTS "Build the headless tests in tests/" OFF)
if(STV_BUILD_TESTS)
    add_subdirectory(tests)
endif()
