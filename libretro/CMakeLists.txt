configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/config_libretro.h
    ${CMAKE_BINARY_DIR}/config.h COPYONLY
)

add_library(mednafen_stv_libretro SHARED
    ${CMAKE_CURRENT_SOURCE_DIR}/libretro.cpp
)

target_include_directories(mednafen_stv_libretro BEFORE PRIVATE
    ${CMAKE_SOURCE_DIR}/libretro/wrapper_include
    ${CMAKE_BINARY_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/minilzo
)

target_compile_options(mednafen_stv_libretro PRIVATE
    -include ${CMAKE_BINARY_DIR}/config.h
)

target_compile_definitions(mednafen_stv_libretro PRIVATE
    HAVE_CONFIG_H=1
    LIBRETRO_BUILD=1
    __LIBRETRO__=1
    WANT_SS_EMU=1
    MDFN_DISABLE_PICPIE_ERRWARN=1
)

find_library(ZSTD_LIB zstd)
target_link_libraries(mednafen_stv_libretro PRIVATE mdfn_core ${ZSTD_LIB})

set_target_properties(mednafen_stv_libretro PROPERTIES
    C_VISIBILITY_PRESET   hidden
    CXX_VISIBILITY_PRESET hidden
    PREFIX ""
)

install(TARGETS mednafen_stv_libretro LIBRARY DESTINATION lib/libretro)
