/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <wdm.h>
// cr_sleep: sleep for specified microseconds
// Behavior:
// - Very short delays use KeStallExecutionProcessor (busy-wait) up to STALL_THRESHOLD_US
// - Longer delays use KeDelayExecutionThread when IRQL < DISPATCH_LEVEL
// - If IRQL >= DISPATCH_LEVEL, fallback to repeated KeStallExecutionProcessor (cannot suspend thread)
// - Uses chunking to avoid 64-bit overflow when converting to 100ns units
STATIC inline VOID
cr_sleep(ULONGLONG us)
{
  const ULONGLONG STALL_THRESHOLD_US = 20ULL; // below or equal this use busy-wait
  const ULONG STALL_CHUNK_MAX_US = 10000UL;  // max chunk for KeStallExecutionProcessor in loop
  const ULONGLONG MAX_100NS = 0x7FFFFFFFFFFFFFFFULL; // max positive 64-bit
  const ULONGLONG MAX_US_FOR_KEDELAY = MAX_100NS / 10ULL; // avoid (us*10) overflow

  if (us == 0) {
    return;
  }

  if (us <= STALL_THRESHOLD_US) {
    KeStallExecutionProcessor((ULONG)us);
    return;
  }

  KIRQL curIrql = KeGetCurrentIrql();

  if (curIrql >= DISPATCH_LEVEL) {
    // Cannot call KeDelayExecutionThread at this IRQL. Fallback to busy-wait in chunks.
    while (us) {
      ULONG chunk = (us > STALL_CHUNK_MAX_US) ? STALL_CHUNK_MAX_US : (ULONG)us;
      KeStallExecutionProcessor(chunk);
      us -= chunk;
    }
    return;
  }

  // IRQL is OK for KeDelayExecutionThread. Use chunking to avoid overflow when converting to 100ns units.
  while (us) {
    ULONGLONG chunk = (us > MAX_US_FOR_KEDELAY) ? MAX_US_FOR_KEDELAY : us;
    LARGE_INTEGER interval;
    // KeDelayExecutionThread expects a relative time in 100-nanosecond units (negative)
    interval.QuadPart = -((LONGLONG)chunk * 10LL); // us * 10 = 100ns units
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
    us -= chunk;
  }
}

#else
#include <Library/TimerLib.h>
#define cr_sleep(us)                                                           \
  do {                                                                         \
    MicroSecondDelay(us);                                                      \
  } while (0)
#endif