/** @file
 *  Cross-platform 32-bit atomic operations.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_types.h>

#ifdef _KERNEL_MODE
#include <ntddk.h>

STATIC inline UINT32
CrAtomicLoad32(IN volatile UINT32 *Value)
{
  return (UINT32)InterlockedCompareExchange(
      (volatile LONG *)Value, 0, 0);
}

STATIC inline UINT32
CrAtomicOr32(IN OUT volatile UINT32 *Value, IN UINT32 Mask)
{
  return (UINT32)InterlockedOr((volatile LONG *)Value, (LONG)Mask);
}

STATIC inline UINT32
CrAtomicAnd32(IN OUT volatile UINT32 *Value, IN UINT32 Mask)
{
  return (UINT32)InterlockedAnd((volatile LONG *)Value, (LONG)Mask);
}

#else
#include <Library/SynchronizationLib.h>

STATIC inline UINT32
CrAtomicLoad32(IN volatile UINT32 *Value)
{
  return InterlockedCompareExchange32(Value, 0, 0);
}

STATIC inline UINT32
CrAtomicOr32(IN OUT volatile UINT32 *Value, IN UINT32 Mask)
{
  UINT32 OldValue;
  UINT32 Observed;

  OldValue = CrAtomicLoad32(Value);
  do {
    Observed = InterlockedCompareExchange32(
        Value, OldValue | Mask, OldValue);
    if (Observed == OldValue) {
      return OldValue;
    }
    OldValue = Observed;
  } while (TRUE);
}

STATIC inline UINT32
CrAtomicAnd32(IN OUT volatile UINT32 *Value, IN UINT32 Mask)
{
  UINT32 OldValue;
  UINT32 Observed;

  OldValue = CrAtomicLoad32(Value);
  do {
    Observed = InterlockedCompareExchange32(
        Value, OldValue & Mask, OldValue);
    if (Observed == OldValue) {
      return OldValue;
    }
    OldValue = Observed;
  } while (TRUE);
}

#endif
