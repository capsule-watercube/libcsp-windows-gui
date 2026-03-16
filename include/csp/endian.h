#ifndef ENDIAN_H_SHIM
#define ENDIAN_H_SHIM

#include <stdint.h>

/* MinGW/Windows equivalent for endian.h constants */
#if defined(_WIN32) || defined(__CYGWIN__)
    #define LITTLE_ENDIAN 1234
    #define BIG_ENDIAN    4321
    #define BYTE_ORDER    LITTLE_ENDIAN

    /* Logic: Since Windows is almost exclusively Little Endian, 
       we define host-to-big-endian as a byte swap. */

    #if defined(_MSC_VER)
        /* Visual Studio Intrinsics */
        #include <stdlib.h>
        #define htobe16(x) _byteswap_ushort(x)
        #define htobe32(x) _byteswap_ulong(x)
        #define be16toh(x) _byteswap_ushort(x)
        #define be32toh(x) _byteswap_ulong(x)
    #elif defined(__GNUC__) || defined(__clang__)
        /* GCC/Clang Intrinsics */
        #define htobe16(x) __builtin_bswap16(x)
        #define htobe32(x) __builtin_bswap32(x)
        #define htobe64(x) __builtin_bswap64(x)
        #define be16toh(x) __builtin_bswap16(x)
        #define be32toh(x) __builtin_bswap32(x)
        #define be64toh(x) __builtin_bswap64(x)
    #else
        /* Manual fallback if no intrinsics available */
        static inline uint16_t swap16(uint16_t x) { return (x << 8) | (x >> 8); }
        static inline uint32_t swap32(uint32_t x) { 
            return ((x << 24) | ((x << 8) & 0xff0000) | ((x >> 8) & 0xff00) | (x >> 24)); 
        }
        #define htobe16(x) swap16(x)
        #define htobe32(x) swap32(x)
        #define be16toh(x) swap16(x)
        #define be32toh(x) swap32(x)
    #endif

#else
    /* Non-Windows: Just use the system header */
    #include <endian.h>
#endif

#ifdef _WIN32
    /* Host to Little Endian (Windows is already Little Endian) */
    #define htole16(x) (x)
    #define htole32(x) (x)
    
    /* Little Endian to Host */
    #define le16toh(x) (x)
    #define le32toh(x) (x)

    /* If CSP uses 64-bit */
    #define htole64(x) (x)
    #define le64toh(x) (x)
#endif

#endif

