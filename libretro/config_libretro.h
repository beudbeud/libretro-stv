/* config_libretro.h — handcrafted config.h for libretro build (no autotools)
 *
 * CRITICAL: This file is force-included (-include) before every translation
 * unit. It must:
 *  1. NOT include any system header that opens extern "C" (no #include <endian.h>)
 *  2. Define INLINE before endian.h is parsed
 *  3. Define all autotools-provided macros
 */
#pragma once

/* ── Package ─────────────────────────────────────────────────────────────── */
#define PACKAGE                     "mednafen"
#define PACKAGE_VERSION             "1.32.1"
#define MEDNAFEN_VERSION            "1.32.1"
#define MEDNAFEN_VERSION_NUMERIC    0x00132100UL
#define PACKAGE_STRING              "mednafen 1.32.1"

/* ── Filesystem path separator (1=Unix, 2=Win, 4=Mac) ───────────────────── */
#define MDFN_PSS_STYLE 1

/* ── Endianness — detect WITHOUT including <endian.h> (it opens extern "C")
 * Use compiler built-ins instead.                                           */
#if defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__)
# if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#  define MSB_FIRST 1
#  define MDFN_IS_BIGENDIAN 1
# else
#  define LSB_FIRST 1
#  define MDFN_IS_BIGENDIAN 0
# endif
#else
/* Fallback: assume little-endian (x86, ARM, AArch64) */
# define LSB_FIRST 1
# define MDFN_IS_BIGENDIAN 0
#endif

/* ── INLINE and related — must be defined BEFORE types.h / endian.h ──────
 * types.h defines these based on __clang__ / __GNUC__, but endian.h uses
 * INLINE before types.h is ever included in some translation units.
 * Defining them here ensures they are always available first.              */
#if defined(__clang__) || defined(__GNUC__)
# define INLINE        inline __attribute__((always_inline))
# define NO_INLINE     __attribute__((noinline))
# define MDFN_COLD     __attribute__((cold))
# define MDFN_HOT      __attribute__((hot))
# define MDFN_HIDE     __attribute__((visibility("hidden")))
# define MDFN_RESTRICT __restrict__
# define MDFN_UNLIKELY(n)  __builtin_expect((n) != 0, 0)
# define MDFN_LIKELY(n)    __builtin_expect((n) != 0, 1)
# define MDFN_WARN_UNUSED_RESULT __attribute__((warn_unused_result))
# define MDFN_NOWARN_UNUSED      __attribute__((unused))
# define MDFN_FORMATSTR(a,b,c)
# define MDFN_ASSUME_ALIGNED(p, align) \
         ((decltype(p))__builtin_assume_aligned((p), (align)))
# define MDFN_UNDEFINED(cond) ((cond) ? (void)__builtin_unreachable() : (void)0)
# if defined(__x86_64__) || defined(__i386__)
#  define MDFN_FASTCALL
# elif defined(__arm__) || defined(__aarch64__)
#  define MDFN_FASTCALL
# else
#  define MDFN_FASTCALL
# endif
# if defined(__GNUC__) && !defined(__clang__)
#  define NO_CLONE __attribute__((noclone))
# else
#  define NO_CLONE
# endif
#else
# define INLINE        inline
# define NO_INLINE
# define NO_CLONE
# define MDFN_COLD
# define MDFN_HOT
# define MDFN_HIDE
# define MDFN_RESTRICT
# define MDFN_UNLIKELY(n)  (n)
# define MDFN_LIKELY(n)    (n)
# define MDFN_WARN_UNUSED_RESULT
# define MDFN_NOWARN_UNUSED
# define MDFN_FORMATSTR(a,b,c)
# define MDFN_ASSUME_ALIGNED(p, align) (p)
# define MDFN_UNDEFINED(cond)
# define MDFN_FASTCALL
#endif

/* ── iconv — on Linux/glibc the 2nd arg is non-const char** ─────────────── */
#define ICONV_CONST

/* ── Standard POSIX headers available ───────────────────────────────────── */
#define HAVE_ALLOCA_H           1
#define HAVE_INTTYPES_H         1
#define HAVE_STDINT_H           1
#define HAVE_STRING_H           1
#define HAVE_STRINGS_H          1
#define HAVE_UNISTD_H           1
#define HAVE_FCNTL_H            1
#define HAVE_SYS_STAT_H         1
#define HAVE_SYS_TYPES_H        1
#define HAVE_PTHREAD_H          1
#define HAVE_ICONV_H            1
#define HAVE_DIRENT_H           1

/* ── POSIX functions ─────────────────────────────────────────────────────── */
#define HAVE_FSEEKO             1
#define HAVE_FTELLO             1
#define HAVE_MKDIR              1
#define HAVE_GETTIMEOFDAY       1
#define HAVE_CLOCK_GETTIME      1
#define HAVE_NANOSLEEP          1
#define HAVE_SETENV             1
#define HAVE_STRDUP             1
#define HAVE_STRNDUP            1
#define HAVE_STRUPR             0
#define HAVE_STRLCPY            0
#define HAVE_PTHREAD_SETNAME_NP 1
#define HAVE_PTHREAD_SET_NAME_NP 0

/* ── Compiler builtins ───────────────────────────────────────────────────── */
#define HAVE___BUILTIN_EXPECT   1
#define HAVE___BUILTIN_POPCOUNT 1
#define HAVE___BUILTIN_CLZ      1
#define HAVE_INLINEASM          1

/* ── 128-bit integers ────────────────────────────────────────────────────── */
#if defined(__SIZEOF_INT128__)
# define HAVE_NATIVE_INT128     1
#endif

/* ── Libraries ───────────────────────────────────────────────────────────── */
#define HAVE_LIBZ               1
#define HAVE_ZSTD               1
#define HAVE_LIBICONV           1

/* Disabled in libretro build */
#define HAVE_ALSA               0
#define HAVE_JACK               0
#define HAVE_OSSAUDIO           0
#define HAVE_SDL                0
#define HAVE_OPENGL             0
#define HAVE_LIBSNDFILE         0
#define HAVE_LIBCDIO            0
#define HAVE_LIBVORBISFILE      0
#define HAVE_FLAC               0
#define HAVE_LIBMPCDEC          0

/* ── MiniLZO ─────────────────────────────────────────────────────────────── */
#define MINILZO_HAVE_CONFIG_H   1

/* ── Mednafen modules — only SS/STV ─────────────────────────────────────── */
#define WANT_SS_EMU             1

/* ── Libretro build marker ───────────────────────────────────────────────── */
#define LIBRETRO_BUILD          1

/* Include mednafen types.h to define uint8/uint16/uint32/uint64,
 * MDFN_bswap*, and include endian.h.
 * Safe because src/ is in -iquote (not -I): sys/types.h doing
 * #include <endian.h> finds the REAL system endian.h, not ours. */

#ifdef __cplusplus
# include "types.h"
#endif

/* ── Type sizes (normally from autotools AC_CHECK_SIZEOF) ─────────────────── */
#define SIZEOF_CHAR         1
#define SIZEOF_SHORT        2
#define SIZEOF_INT          4
#define SIZEOF_LONG         8
#define SIZEOF_LONG_LONG    8
#define SIZEOF_OFF_T        8
#define SIZEOF_PTRDIFF_T    8
#define SIZEOF_SIZE_T       8
#define SIZEOF_VOID_P       8
