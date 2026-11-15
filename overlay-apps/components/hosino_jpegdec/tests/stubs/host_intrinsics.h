#pragma once
#include <stdint.h>
/* TCC's Windows headers define _M_X64 but don't export MSVC intrinsics. */
static inline uint16_t _byteswap_ushort(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
static inline uint32_t _byteswap_ulong(uint32_t v) {
    return ((v >> 24) & 0xffu) | ((v >> 8) & 0xff00u) |
           ((v << 8) & 0xff0000u) | ((v << 24) & 0xff000000u);
}
static inline uint64_t _byteswap_uint64(uint64_t v) {
    return ((uint64_t)_byteswap_ulong((uint32_t)v) << 32) | _byteswap_ulong((uint32_t)(v >> 32));
}
/* Exercise the same 32-bit bitstream-reader branch as Xtensa, despite the
 * host process having 64-bit pointers. This does not emulate S3 SIMD. */
#undef _M_X64
#undef __x86_64__
#define __builtin_bswap16 _byteswap_ushort
#define __builtin_bswap32 _byteswap_ulong
#define __builtin_bswap64 _byteswap_uint64
