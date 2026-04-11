cmake_minimum_required(VERSION 3.16)
project(mednafen_stv VERSION 1.32.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -fsigned-char -fwrapv -fexceptions
    -fno-fast-math -fno-unsafe-math-optimizations
    -fno-aggressive-loop-optimizations -fno-ipa-icf
    -fno-printf-return-value -fstrict-aliasing
)

add_compile_definitions(
    _LARGEFILE_SOURCE _LARGEFILE64_SOURCE _FILE_OFFSET_BITS=64
    HAVE_CONFIG_H
    $<$<CONFIG:Release>:_FORTIFY_SOURCE=1>
)

configure_file(${CMAKE_SOURCE_DIR}/libretro/config_libretro.h
               ${CMAKE_BINARY_DIR}/config.h COPYONLY)

add_subdirectory(src)
add_subdirectory(libretro)
