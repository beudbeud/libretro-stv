# src/CMakeLists.txt — mednafen core for libretro build

# ── Source collection ─────────────────────────────────────────────────────────
file(GLOB MDFN_CORE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
file(GLOB_RECURSE MDFN_SS_SRCS       ${CMAKE_CURRENT_SOURCE_DIR}/ss/*.cpp)
file(GLOB_RECURSE MDFN_CDROM_SRCS    ${CMAKE_CURRENT_SOURCE_DIR}/cdrom/*.cpp)
file(GLOB_RECURSE MDFN_VIDEO_SRCS    ${CMAKE_CURRENT_SOURCE_DIR}/video/*.cpp)
file(GLOB_RECURSE MDFN_SOUND_SRCS    ${CMAKE_CURRENT_SOURCE_DIR}/sound/*.cpp)
file(GLOB_RECURSE MDFN_COMPRESS_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/compress/*.cpp)
file(GLOB_RECURSE MDFN_STRING_SRCS   ${CMAKE_CURRENT_SOURCE_DIR}/string/*.cpp)
file(GLOB_RECURSE MDFN_HASH_SRCS     ${CMAKE_CURRENT_SOURCE_DIR}/hash/*.cpp)
file(GLOB_RECURSE MDFN_TIME_SRCS     ${CMAKE_CURRENT_SOURCE_DIR}/time/*.cpp)
file(GLOB_RECURSE MDFN_RESAMPLER_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/resampler/*.cpp)
file(GLOB_RECURSE MDFN_HW_CPU_SRCS   ${CMAKE_CURRENT_SOURCE_DIR}/hw_cpu/*.cpp)
file(GLOB_RECURSE MDFN_HW_MISC_SRCS  ${CMAKE_CURRENT_SOURCE_DIR}/hw_misc/*.cpp)
file(GLOB_RECURSE MDFN_HW_SOUND_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/hw_sound/*.cpp)
file(GLOB_RECURSE MDFN_HW_VIDEO_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/hw_video/*.cpp)

# ── Exclusions (MUST come after GLOB) ────────────────────────────────────────

# Standalone driver sources
list(FILTER MDFN_CORE_SRCS EXCLUDE REGEX ".*/drivers/.*")

# Windows-only sources — apply to all lists
foreach(_SL MDFN_CORE_SRCS MDFN_SS_SRCS MDFN_CDROM_SRCS MDFN_VIDEO_SRCS
            MDFN_SOUND_SRCS MDFN_COMPRESS_SRCS MDFN_STRING_SRCS MDFN_HASH_SRCS
            MDFN_TIME_SRCS MDFN_RESAMPLER_SRCS MDFN_HW_CPU_SRCS MDFN_HW_MISC_SRCS
            MDFN_HW_SOUND_SRCS MDFN_HW_VIDEO_SRCS)
    list(FILTER ${_SL} EXCLUDE REGEX ".*[Ww]in32.*\\.cpp$")
    list(FILTER ${_SL} EXCLUDE REGEX ".*_Win32\\.cpp$")
    list(FILTER ${_SL} EXCLUDE REGEX ".*_win32\\.cpp$")
endforeach()

# Source-specific exclusions
list(FILTER MDFN_CORE_SRCS  EXCLUDE REGEX ".*/tests\\.cpp$")
list(FILTER MDFN_CORE_SRCS  EXCLUDE REGEX ".*/testsexp\\.cpp$")
list(FILTER MDFN_CORE_SRCS  EXCLUDE REGEX ".*/qtrecord\\.cpp$")
list(FILTER MDFN_SOUND_SRCS EXCLUDE REGEX ".*/okiadpcm_generate\\.cpp$")

# CD audio decoders that need optional libs not available on embedded targets
list(FILTER MDFN_CDROM_SRCS EXCLUDE REGEX ".*/CDAFReader_MPC\\.cpp$")
list(FILTER MDFN_CDROM_SRCS EXCLUDE REGEX ".*/CDAFReader_Vorbis\\.cpp$")
list(FILTER MDFN_CDROM_SRCS EXCLUDE REGEX ".*/CDAFReader_FLAC\\.cpp$")

# ── Library ───────────────────────────────────────────────────────────────────
add_library(mdfn_core STATIC
    # C sources (not picked up by *.cpp globs)
    ${CMAKE_CURRENT_SOURCE_DIR}/minilzo/minilzo.c
    ${CMAKE_CURRENT_SOURCE_DIR}/resampler/resample.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trio/trio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trio/trionan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/trio/triostr.c
    # POSIX-specific implementations
    ${CMAKE_CURRENT_SOURCE_DIR}/net/Net.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/net/Net_POSIX.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/mthreading/MThreading_POSIX.cpp
    # Stubs for excluded decoders
    ${CMAKE_CURRENT_SOURCE_DIR}/cdrom/CDAFReader_stubs.cpp
    # All C++ sources
    ${MDFN_CORE_SRCS}
    ${MDFN_SS_SRCS}
    ${MDFN_CDROM_SRCS}
    ${MDFN_VIDEO_SRCS}
    ${MDFN_SOUND_SRCS}
    ${MDFN_COMPRESS_SRCS}
    ${MDFN_STRING_SRCS}
    ${MDFN_HASH_SRCS}
    ${MDFN_TIME_SRCS}
    ${MDFN_RESAMPLER_SRCS}
    ${MDFN_HW_CPU_SRCS}
    ${MDFN_HW_MISC_SRCS}
    ${MDFN_HW_SOUND_SRCS}
    ${MDFN_HW_VIDEO_SRCS}
)

set_target_properties(mdfn_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_source_files_properties(ss/cart/stv.cpp PROPERTIES COMPILE_FLAGS "-fPIC")

# ── Includes ──────────────────────────────────────────────────────────────────
target_include_directories(mdfn_core BEFORE PUBLIC
    ${CMAKE_SOURCE_DIR}/libretro/wrapper_include  # intercepts <endian.h>
    ${CMAKE_BINARY_DIR}                            # <config.h>
    ${CMAKE_SOURCE_DIR}/src                        # <trio/trio.h>, <mednafen/X.h> via proxy
    ${CMAKE_SOURCE_DIR}/src/minilzo                # internal minilzo headers
)

target_compile_options(mdfn_core PUBLIC
    -include ${CMAKE_BINARY_DIR}/config.h
)

target_compile_definitions(mdfn_core PUBLIC
    HAVE_CONFIG_H=1
    WANT_SS_EMU=1
    MDFN_DISABLE_PICPIE_ERRWARN=1
    LIBRETRO_BUILD=1
    __LIBRETRO__=1
)

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_library(ZSTD_LIB zstd REQUIRED)

target_link_libraries(mdfn_core PUBLIC
    ZLIB::ZLIB
    Threads::Threads
    ${ZSTD_LIB}
)
