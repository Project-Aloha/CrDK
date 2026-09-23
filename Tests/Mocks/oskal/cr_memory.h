/** @file
 *  Host-test MMIO overrides for libraries which contain AArch64 barriers.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_types.h>

#define CR_MEM_BARRIER_DATA_SYN_BARRIAR() \
  do {                                      \
    __asm__ volatile("" ::: "memory");      \
  } while (0)

UINT32
CrMmioRead32(IN UINTN Address);

UINT32
CrMmioWrite32(IN UINTN Address, IN UINT32 Value);
