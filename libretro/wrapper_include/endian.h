/* endian.h — interception wrapper for mednafen libretro build.
 *
 * Intercepted so that sys/types.h (inside extern "C") resolves to this file
 * instead of mednafen/endian.h (which contains C++ templates).
 *
 * On glibc: reconstruct via bits/endian.h.
 * On Android/bionic: pass through to the system endian.h directly.
 */
#ifndef _WRAPPER_ENDIAN_H
#define _WRAPPER_ENDIAN_H 1

#ifdef __ANDROID__
# include_next <endian.h>
#else
# ifndef _ENDIAN_H
# define _ENDIAN_H 1

# include <features.h>
# include <bits/endian.h>

# ifdef __USE_MISC
#  define LITTLE_ENDIAN __LITTLE_ENDIAN
#  define BIG_ENDIAN    __BIG_ENDIAN
#  define PDP_ENDIAN    __PDP_ENDIAN
#  define BYTE_ORDER    __BYTE_ORDER
# endif

# if defined __USE_MISC && !defined __ASSEMBLER__
#  include <bits/byteswap.h>
#  include <bits/uintn-identity.h>

#  if __BYTE_ORDER == __LITTLE_ENDIAN
#   define htobe16(x) __bswap_16(x)
#   define htole16(x) __uint16_identity(x)
#   define be16toh(x) __bswap_16(x)
#   define le16toh(x) __uint16_identity(x)
#   define htobe32(x) __bswap_32(x)
#   define htole32(x) __uint32_identity(x)
#   define be32toh(x) __bswap_32(x)
#   define le32toh(x) __uint32_identity(x)
#   define htobe64(x) __bswap_64(x)
#   define htole64(x) __uint64_identity(x)
#   define be64toh(x) __bswap_64(x)
#   define le64toh(x) __uint64_identity(x)
#  else
#   define htobe16(x) __uint16_identity(x)
#   define htole16(x) __bswap_16(x)
#   define be16toh(x) __uint16_identity(x)
#   define le16toh(x) __bswap_16(x)
#   define htobe32(x) __uint32_identity(x)
#   define htole32(x) __bswap_32(x)
#   define be32toh(x) __uint32_identity(x)
#   define le32toh(x) __bswap_32(x)
#   define htobe64(x) __uint64_identity(x)
#   define htole64(x) __bswap_64(x)
#   define be64toh(x) __uint64_identity(x)
#   define le64toh(x) __bswap_64(x)
#  endif
# endif

# endif /* _ENDIAN_H */
#endif /* __ANDROID__ */

#endif /* _WRAPPER_ENDIAN_H */
