/** @file Kernel query-interface ABI for the Crane clock provider.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <wdm.h>
#include <Library/clock.h>

#define WDF_CLOCK_CR_INTERFACE_REVISION 2U
DEFINE_GUID(GUID_DEVINTERFACE_CLOCK_CR, 0x98792c87, 0xdf2e, 0x477b,
            0x8e, 0x97, 0xe4, 0xe1, 0x6a, 0x4d, 0xdb, 0x98);
DEFINE_GUID(GUID_CLOCK_CR_INTERFACE, 0x1657d941, 0xab1a, 0x42f2,
            0xb2, 0xc9, 0xd4, 0x12, 0x95, 0x70, 0x20, 0x8d);

/* PASSIVE_LEVEL, kernel callers only. Each successful enable acquires one
 * reference; callers balance it with disable before releasing the interface.
 * Rates are Hz and controller IDs are ClockControllerType, not DT clock IDs.
 * Releasing the provider drains requests and restores all remaining votes.
 */
typedef NTSTATUS (*CLOCK_CR_SET_CLOCK)(VOID *Context, UINT32 Controller,
    CONST CHAR8 *Name, UINT64 RateHz, BOOLEAN Enable);
typedef NTSTATUS (*CLOCK_CR_SET_GDSC)(VOID *Context, UINT32 Controller,
    CONST CHAR8 *Name, BOOLEAN Enable);
typedef NTSTATUS (*CLOCK_CR_RESET)(VOID *Context, CONST CHAR8 *Controller,
    CONST CHAR8 *Name, BOOLEAN Assert);
/* Configure gated RCGs before SetClock. Divider uses HID encoding (twice the
 * source divisor); separate post-dividers use their integer divisor. A mux
 * accepts source 0 (PHY) or 2 (XO) and may switch one live owner's parent. */
typedef NTSTATUS (*CLOCK_CR_CONFIGURE_SOURCE)(VOID *Context, UINT32 Controller,
    CONST CHAR8 *Name, UINT32 Source, UINT32 Divider,
    UINT32 M, UINT32 N, UINT32 TwiceD);
typedef NTSTATUS (*CLOCK_CR_SET_DIVIDER)(VOID *Context, UINT32 Controller,
    CONST CHAR8 *Name, UINT32 Divider);
typedef struct {
  INTERFACE Header;
  CLOCK_CR_SET_CLOCK SetClock;
  CLOCK_CR_SET_GDSC SetGdsc;
  CLOCK_CR_RESET Reset;
  CLOCK_CR_CONFIGURE_SOURCE ConfigureSource;
  CLOCK_CR_SET_DIVIDER SetDivider;
} WDF_CLOCK_CR_INTERFACE;
