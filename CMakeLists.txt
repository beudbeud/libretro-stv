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

project(mednafen_stv VERSION ${GIT_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -fsigned-char -fwrapv -fexceptions
    -fno-fast-math -fno-unsafe-math-optimizations
    -fno-aggressive-loop-optimizations -fno-ipa-icf
    -fno-printf-return-value -fstrict-aliasing
)

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
