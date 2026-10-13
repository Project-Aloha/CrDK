/** @file Portable SM8450 clock target and per-instance state.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <Library/clock.h>

typedef struct {
  UINTN Address; /* mapped virtual register address */
  UINT32 Size;
} CR_CLOCK_MAPPING;

/* Storage must be pointer-aligned, private to this instance and live until all
 * consumers and MMIO mappings have been released. Frequency tables are immutable.
 * Create/Bind perform no MMIO. Bind validates every window before publishing any.
 */
UINTN CrSm8450ClockStateSize (VOID);
CR_STATUS CrSm8450ClockCreate (VOID *Storage, UINTN Size,
                             ClockDriverContext **Context);
CR_STATUS CrSm8450ClockBind (ClockDriverContext *Context,
                           CONST CR_CLOCK_MAPPING *Mappings, UINTN Count);
CR_STATUS CrSm8450ClockResource (ClockControllerType Type,
                               UINT64 *PhysicalAddress, UINT32 *Size);
CR_STATUS CrSm8450ClockInit (ClockDriverContext *Context);
CR_STATUS CrSm8450ClockReset (ClockDriverContext *Context,
                            CONST CHAR8 *Controller, CONST CHAR8 *Name,
                            BOOLEAN Assert);
/* After all users have drained; release votes before unmapping. A failed
 * release retains graph ownership so the adapter can retry. */
CR_STATUS CrSm8450ClockRelease (ClockDriverContext *Context);
#ifndef _KERNEL_MODE
ClockDriverContext *CrSm8450GetNativeClockContext (VOID);
#endif
