/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

//#define CR_OSKAL_TRACE_MMIO 1

#ifdef CR_OSKAL_TRACE_MMIO
#include <oskal/cr_debug.h>
#endif

#include <oskal/cr_assert.h>
#include <oskal/cr_types.h>

#ifdef _KERNEL_MODE
#include <ntddk.h>
#define CR_MEM_BARRIER_DATA_SYN_BARRIAR() REGISTER_FENCE();
UINT32 CrMmioRead32(UINTN Address);
UINT32 CrMmioWrite32(UINTN Address, UINT32 Value);
UINT32 CrMmioOr32(UINTN Address, UINT32 DataToOr);
UINT32 CrMmioUpdateBits32(UINTN Address, UINT32 Mask, UINT32 Data);
UINT32 CrMmioAnd32(UINTN Address, UINT32 DataToAnd);
#else
#include <Library/IoLib.h>

#define CR_MEM_BARRIER_DATA_SYN_BARRIAR()                                        \
  do {                                                                         \
    asm volatile("dsb sy" ::: "memory");                                       \
  } while (0)

/**
 * CrMmioRead32 - Read a 32-bit value from a MMIO address
 *
 * @param  Address The MMIO address to read
 *
 * @return The value read from the MMIO address
 **/

STATIC inline UINT32 CrMmioRead32(UINTN Address) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO Read32: Address=0x%lx, Value=0x%lx", Address,
           MmioRead32(Address));
  CR_ASSERT(Address != 0);
#endif
  return MmioRead32(Address);
}

/**
 * CrMmioWrite32 - Write a 32-bit value to a MMIO address
 *
 * @param  Address The MMIO address to write
 * @param  Value   The value to write to the MMIO address
 *
 * @return The value written to the MMIO address
 **/
STATIC inline UINT32 CrMmioWrite32(UINTN Address, UINT32 Value) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO Write32: Address=0x%lx, Value=0x%lx", Address, Value);
  CR_ASSERT(Address != 0);
#endif
  return MmioWrite32(Address, Value);
}
/**
 * CrMmioOr32 - Perform a read-modify-write MMIO OR operation
 *
 * @param  Address   The MMIO address to write
 * @param  DataToOr  The value to OR with the current value at Address
 *
 * @return The value written to the MMIO address
 **/
STATIC inline UINT32 CrMmioOr32(UINTN Address, UINT32 DataToOr) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO Or32: Address=0x%lx, DataToOr=0x%lx", Address, DataToOr);
  CR_ASSERT(Address != 0);
#endif
  return MmioOr32(Address, DataToOr);
}

STATIC inline UINT32 CrMmioUpdateBits32(UINTN Address, UINT32 Mask,
                                        UINT32 Data) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO UpdateBits32: Address=0x%lx, Mask=0x%lx, Data=0x%lx", Address,
           Mask, Data);
  CR_ASSERT(Address != 0);
#endif
  UINT32 Val = CrMmioRead32(Address);
  Val &= ~Mask;         // Clear bits specified by Mask
  Val |= (Data & Mask); // Set bits specified by Data
  CrMmioWrite32(Address, Val);
  return Val;
}

/**
 * CrMmioAnd32 - Perform a read-modify-write MMIO AND operation
 *
 * @param  Address    The MMIO address to write
 * @param  DataToAnd  The value to AND with the current value at Address
 *
 * @return The value written to the MMIO address
 **/
STATIC inline UINT32 CrMmioAnd32(UINTN Address, UINT32 DataToAnd) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO And32: Address=0x%lx, DataToAnd=0x%lx", Address, DataToAnd);
  CR_ASSERT(Address != 0);
#endif
  return MmioAnd32(Address, DataToAnd);
}

#endif // _KERNEL_MODE