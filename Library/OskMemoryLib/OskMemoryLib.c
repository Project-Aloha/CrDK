/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#include <oskal/cr_memory.h>

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <oskal/cr_debug.h>
#include "OskMemoryLib.tmh"

UINT32 CrMmioRead32(UINTN Address) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO Read32: Address=0x%llx, Value=0x%lx", Address,
           READ_REGISTER_ULONG((PULONG)(UINTN)Address));
  CR_ASSERT(Address != 0);
#endif
  return READ_REGISTER_ULONG((PULONG)(UINTN)Address);
}

UINT32 CrMmioWrite32(UINTN Address, UINT32 Value) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO Write32: Address=0x%llx, Value=0x%lx", Address, Value);
  CR_ASSERT(Address != 0);
#endif
   WRITE_REGISTER_ULONG((PULONG)(UINTN)Address, Value);
  return Value;
}

UINT32 CrMmioOr32(UINTN Address, UINT32 DataToOr) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO Or32: Address=0x%llx, DataToOr=0x%lx", Address, DataToOr);
  CR_ASSERT(Address != 0);
#endif
  UINT32 Val = CrMmioRead32(Address);
  Val |= DataToOr;
  CrMmioWrite32(Address, Val);
  return Val;
}

UINT32 CrMmioUpdateBits32(UINTN Address, UINT32 Mask,
                                        UINT32 Data) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO UpdateBits32: Address=0x%llx, Mask=0x%lx, Data=0x%lx", Address,
           Mask, Data);
  CR_ASSERT(Address != 0);
#endif
  UINT32 Val = CrMmioRead32(Address);
  Val &= ~Mask;         // Clear bits specified by Mask
  Val |= (Data & Mask); // Set bits specified by Data
  CrMmioWrite32(Address, Val);
  return Val;
}

UINT32 CrMmioAnd32(UINTN Address, UINT32 DataToAnd) {
#ifdef CR_OSKAL_TRACE_MMIO
  log_info("MMIO And32: Address=0x%llx, DataToAnd=0x%lx", Address, DataToAnd);
  CR_ASSERT(Address != 0);
#endif
  UINT32 Val = CrMmioRead32(Address);
  Val &= DataToAnd;
  CrMmioWrite32(Address, Val);
  return Val;
}
#endif