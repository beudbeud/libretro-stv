/* endian.h — interception wrapper for mednafen libretro build.
 *
 * When sys/types.h (inside extern "C") does #include <endian.h>, this file
 * is found first (wrapper_include/ is first in -I order). It provides the
 * real system endian.h functionality WITHOUT triggering src/endian.h
 * (which contains C++ templates incompatible with extern "C").
 *
 * Portable: uses <bits/endian.h> which is always available in the sysroot.
 */
#ifndef _ENDIAN_H
#define _ENDIAN_H 1

#include <features.h>
#include <bits/endian.h>

#ifdef __USE_MISC
# define LITTLE_ENDIAN __LITTLE_ENDIAN
# define BIG_ENDIAN    __BIG_ENDIAN
# define PDP_ENDIAN    __PDP_ENDIAN
# define BYTE_ORDER    __BYTE_ORDER
#endif

#if defined __USE_MISC && !defined __ASSEMBLER__
# include <bits/byteswap.h>
# include <bits/uintn-identity.h>

# if __BYTE_ORDER == __LITTLE_ENDIAN
#  define htobe16(x) __bswap_16(x)
#  define htole16(x) __uint16_identity(x)
#  define be16toh(x) __bswap_16(x)
#  define le16toh(x) __uint16_identity(x)
#  define htobe32(x) __bswap_32(x)
#  define htole32(x) __uint32_identity(x)
#  define be32toh(x) __bswap_32(x)
#  define le32toh(x) __uint32_identity(x)
#  define htobe64(x) __bswap_64(x)
#  define htole64(x) __uint64_identity(x)
#  define be64toh(x) __bswap_64(x)
#  define le64toh(x) __uint64_identity(x)
# else
#  define htobe16(x) __uint16_identity(x)
#  define htole16(x) __bswap_16(x)
#  define be16toh(x) __uint16_identity(x)
#  define le16toh(x) __bswap_16(x)
#  define htobe32(x) __uint32_identity(x)
#  define htole32(x) __bswap_32(x)
#  define be32toh(x) __uint32_identity(x)
#  define le32toh(x) __bswap_32(x)
#  define htobe64(x) __uint64_identity(x)
#  define htole64(x) __bswap_64(x)
#  define be64toh(x) __uint64_identity(x)
#  define le64toh(x) __bswap_64(x)
# endif
#endif

#endif /* endian.h */
