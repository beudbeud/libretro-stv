/* endian.h — interception wrapper for mednafen libretro build.
 *
 * Intercepted so that sys/types.h (inside extern "C") resolves to this file
 * instead of mednafen/endian.h (which contains C++ templates).
 *
 * On Android/bionic: pass through to the system endian.h directly.
 * Elsewhere: provide the definitions <endian.h> is expected to supply,
 * derived from the compiler's own byte-order macros so that the header is
 * not tied to one libc's internals (glibc's bits/endian.h does not exist on
 * musl, for instance).
 */
#ifndef _WRAPPER_ENDIAN_H
#define _WRAPPER_ENDIAN_H 1

#ifdef __ANDROID__
# include_next <endian.h>
#else
/* Claim the include guards of the libc headers we are standing in for, so a
   later #include <endian.h> from libc internals does not pull in the real one
   (which would again be shadowed by mednafen/endian.h). */
# ifndef _ENDIAN_H
#  define _ENDIAN_H 1
# endif
# ifndef _BITS_ENDIAN_H
#  define _BITS_ENDIAN_H 1
# endif

# ifndef __LITTLE_ENDIAN
#  define __LITTLE_ENDIAN 1234
# endif
# ifndef __BIG_ENDIAN
#  define __BIG_ENDIAN 4321
# endif
# ifndef __PDP_ENDIAN
#  define __PDP_ENDIAN 3412
# endif

# ifndef __BYTE_ORDER
#  if defined __BYTE_ORDER__ && defined __ORDER_BIG_ENDIAN__ && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#   define __BYTE_ORDER __BIG_ENDIAN
#  else
#   define __BYTE_ORDER __LITTLE_ENDIAN
#  endif
# endif
# ifndef __FLOAT_WORD_ORDER
#  define __FLOAT_WORD_ORDER __BYTE_ORDER
# endif

# ifndef LITTLE_ENDIAN
#  define LITTLE_ENDIAN __LITTLE_ENDIAN
# endif
# ifndef BIG_ENDIAN
#  define BIG_ENDIAN    __BIG_ENDIAN
# endif
# ifndef PDP_ENDIAN
#  define PDP_ENDIAN    __PDP_ENDIAN
# endif
# ifndef BYTE_ORDER
#  define BYTE_ORDER    __BYTE_ORDER
# endif

# ifndef __ASSEMBLER__
#  ifndef __bswap_16
#   define __bswap_16(x) __builtin_bswap16(x)
#  endif
#  ifndef __bswap_32
#   define __bswap_32(x) __builtin_bswap32(x)
#  endif
#  ifndef __bswap_64
#   define __bswap_64(x) __builtin_bswap64(x)
#  endif

#  if __BYTE_ORDER == __LITTLE_ENDIAN
#   define htobe16(x) __builtin_bswap16(x)
#   define htole16(x) ((uint16_t)(x))
#   define be16toh(x) __builtin_bswap16(x)
#   define le16toh(x) ((uint16_t)(x))
#   define htobe32(x) __builtin_bswap32(x)
#   define htole32(x) ((uint32_t)(x))
#   define be32toh(x) __builtin_bswap32(x)
#   define le32toh(x) ((uint32_t)(x))
#   define htobe64(x) __builtin_bswap64(x)
#   define htole64(x) ((uint64_t)(x))
#   define be64toh(x) __builtin_bswap64(x)
#   define le64toh(x) ((uint64_t)(x))
#  else
#   define htobe16(x) ((uint16_t)(x))
#   define htole16(x) __builtin_bswap16(x)
#   define be16toh(x) ((uint16_t)(x))
#   define le16toh(x) __builtin_bswap16(x)
#   define htobe32(x) ((uint32_t)(x))
#   define htole32(x) __builtin_bswap32(x)
#   define be32toh(x) ((uint32_t)(x))
#   define le32toh(x) __builtin_bswap32(x)
#   define htobe64(x) ((uint64_t)(x))
#   define htole64(x) __builtin_bswap64(x)
#   define be64toh(x) ((uint64_t)(x))
#   define le64toh(x) __builtin_bswap64(x)
#  endif
# endif /* !__ASSEMBLER__ */
#endif /* __ANDROID__ */

#endif /* _WRAPPER_ENDIAN_H */
