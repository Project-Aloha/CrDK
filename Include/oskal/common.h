/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

// Size ops
#define CR_SIZE_1B 1ULL
#define CR_SIZE_1KB (1024ULL * CR_SIZE_1B)
#define CR_SIZE_1MB (1024ULL * CR_SIZE_1KB)
#define CR_SIZE_1GB (1024ULL * CR_SIZE_1MB)
#define CR_SIZE_B(x) ((x) * CR_SIZE_1B)
#define CR_SIZE_KB(x) ((x) * CR_SIZE_1KB)
#define CR_SIZE_MB(x) ((x) * CR_SIZE_1MB)
#define CR_SIZE_GB(x) ((x) * CR_SIZE_1GB)

// Bit ops
#if defined(_MSC_VER)
static inline int __ffsll_ms64(unsigned long long x)
{
  if (x == 0)
    return 0;
  unsigned long index;
  _BitScanForward64(&index, x);
  return (int)index + 1;
}
#define __ffs(x) __ffsll_ms64((unsigned long long)(x))
#else
#define __ffs(x) __builtin_ffsll(x)
#endif

#if defined(_MSC_VER)
static inline unsigned ctzll(unsigned long long x) {
    unsigned long index;
    if (_BitScanForward64(&index, x))
        return index;
    return 64;
}
#define __ctzll(x) ctzll((unsigned long long)(x))
#else
#define __ctzll __builtin_ctzll
#endif



#define BIT(x) (1ULL << (x))
#define GEN_MSK(high, low) (((BIT((high) + 1) - 1) & ~((BIT(low)) - 1)))

#define SET_BITS(val, mask) ((val) | (unsigned long long)(mask))
#define CLR_BITS(val, mask) ((val) & ~(unsigned long long)(mask))

#define SET_FIELD(val, mask)                                                   \
  (((unsigned long long)(val)                                                  \
    << __ctzll((unsigned long long)(mask))) &                          \
   (unsigned long long)(mask))

#define GET_FIELD(val, mask) (((val) & (mask)) >> (__ffs(mask) - 1))

// ARM GIC related conversion macros
// x means the irq number in device tree.
#define GIC_SGI(x) (x)
#define GIC_PPI(x) (16ULL + (x))
#define GIC_SPI(x) (32ULL + (x))
#define GIC_LPI(x) (8192ULL + (x))

// Declare a flexible array member in a struct
#ifndef ANYSIZE_ARRAY
#define ANYSIZE_ARRAY 1
#endif
#define CR_FLEXIBLE_ARRAY_MEMBER(type, name) type name[ANYSIZE_ARRAY]
#define CR_FLEXIBLE_ARRAY_SIZE(type) (sizeof(type) * ANYSIZE_ARRAY)
