/** @file
  Portable target-data ABI for a memory-mapped TRNG.

  SPDX-License-Identifier: MIT
**/
#ifndef CR_TARGET_TRNG_LIB_H_
#define CR_TARGET_TRNG_LIB_H_

#include <Base.h>

typedef struct {
  UINTN   BaseAddress;
  UINTN   MmioSize;
  UINTN   DataOutOffset;
  UINTN   StatusOffset;
  UINT32  DataAvailableMask;
  UINTN   MaxEntropyBits;
  UINTN   PollDelayUs;
  UINTN   TimeoutUs;
} CR_TRNG_CONFIG;

CONST CR_TRNG_CONFIG *
CrTargetGetTrngConfig (
  VOID
  );

#endif
