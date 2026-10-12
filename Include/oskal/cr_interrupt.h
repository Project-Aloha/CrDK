/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <oskal/cr_interrupt.h>

typedef enum {
  CR_INTERRUPT_TRIGGER_LEVEL_LOW,
  CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
  CR_INTERRUPT_TRIGGER_EDGE_FALLING,
  CR_INTERRUPT_TRIGGER_EDGE_RISING,
  CR_INTERRUPT_TRIGGER_EDGE_BOTH,
  CR_INTERRUPT_TRIGGER_TYPE_INVALID = 0xFF
} CR_INTERRUPT_TRIGGER_TYPE;

typedef VOID (*CR_INTERRUPT_HANDLER)(VOID *Parameters);

typedef struct {
  CR_INTERRUPT_HANDLER Handler;
  VOID *Param;
  UINT64 Source;
} CR_INTERRUPT_ENTRY;
// 16 is enough for a single driver linked to this library.
#define MAX_INTERRUPT_ENTRIES 16

#ifdef _KERNEL_MODE
#include <wdf.h>
typedef enum {
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_NONE = 0,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_DPC,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_WORKITEM
} OSKAL_INTERRUPT_L_IRQ_LCB_TYPE;
struct _CR_INTERRUPT_CONFIG;
/* An OS adapter may supply a non-framework interrupt backend. These callbacks
 * have the same exact-owner and synchronous-drain contract as OSKAL. A backend
 * is immutable through successful unregister; NULL selects the WDF backend.
 * This permits a permanent WDM PEP host without a hidden KMDF parent. */
typedef struct {
  VOID *Context;
  CR_STATUS (*Register)(VOID *,struct _CR_INTERRUPT_CONFIG *);
  CR_STATUS (*Unregister)(VOID *,struct _CR_INTERRUPT_CONFIG *);
  BOOLEAN (*IsRegistered)(VOID *,struct _CR_INTERRUPT_CONFIG *);
} CR_INTERRUPT_BACKEND;
typedef struct _CR_INTERRUPT_CONFIG {
  VOID *Param;
  CR_INTERRUPT_HANDLER Handler;
  // Initialized WDF interrupt config by evt device prepare hardware function
  WDF_INTERRUPT_CONFIG WdfInterruptConfig;
  WDF_OBJECT_ATTRIBUTES WdfInterruptAttributes;
  // Interrupt handle
  WDFINTERRUPT Interrupt;
  // Device handle
  WDFDEVICE Device;
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE LIrqLCbType;
  WDFWORKITEM WorkItem;
  CONST CR_INTERRUPT_BACKEND *Backend;
} CR_INTERRUPT_CONFIG;
#else

enum {
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_NONE = 0,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_DPC,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_WORKITEM
};

typedef struct {
  VOID *Param;
  CR_INTERRUPT_HANDLER Handler;
  UINT32 InterruptNumber;
  CR_INTERRUPT_TRIGGER_TYPE TriggerType;
} CR_INTERRUPT_CONFIG;
#endif

/**
 * Register an interrupt handler for a specific interrupt vector
 *
 * @param InterruptNumber  The interrupt vector number to register
 * @param InterruptHandler The handler function to call when interrupt fires,
 * disable irq if null.
 * @param Param            Parameter to pass to the interrupt handler
 * @param TriggerType      Interrupt trigger type (edge or level)
 *
 * @return CR_SUCCESS if successful, error code otherwise
 */
CR_STATUS
CrRegisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig);

/**
 * Unregister an interrupt handler
 * Success means callbacks have been removed synchronously. Kernel-mode
 * callers run this in their PASSIVE_LEVEL teardown path; higher IRQL returns
 * CR_BUSY while retaining the interrupt handle and callback ownership.
 * WDF keeps its framework-owned object until device removal. A successful
 * unregister detaches the callback and retains Interrupt for a later rebind.
 * Registration/removal run in serialized PnP/power callbacks; WDF owns D0
 * activation. Do not free OSKAL code while framework callbacks still exist.
 *
 * @param InterruptNumber  The interrupt vector number to unregister
 *
 * @return CR_SUCCESS if successful, error code otherwise
 */
CR_STATUS
CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig);

/* Query only a matching handler and context owned by this OSKAL
   instance. A failed registration rollback can retain that ownership. */
BOOLEAN
CrInterruptIsRegistered(IN CR_INTERRUPT_CONFIG *InterruptConfig);

#ifndef _KERNEL_MODE
/* Native route/table mutation excludes CPU IRQ dispatch and deferred events.
 * Cold setup validates HardwareInterrupt2 at <=TPL_NOTIFY before raising
 * HIGH; a cold call already above NOTIFY declines without protocol lookup.
 * Nested setup scopes reuse the cache; entry from a running OSKAL ISR fails
 * busy. Windows adapters use their class/WDF synchronization instead. */
typedef struct { UINTN PreviousPriority; BOOLEAN Active; } CR_INTERRUPT_GUARD;
CR_STATUS CrInterruptGuardAcquire(OUT CR_INTERRUPT_GUARD *Guard);
VOID CrInterruptGuardRelease(IN OUT CR_INTERRUPT_GUARD *Guard);
#endif

/* Deferred native IRQ service lease. Generation belongs to this OSKAL image,
 * not the hardware vector. Config storage/handler/context must remain pinned
 * through successful unregister and all consumer deferred work. GetLease is
 * outside ISR; Mask may run only in this lease's own ISR or guarded native
 * owner path. Unmask is outside ISR. Both verify the actual GIC enable state.
 * Unregister invalidates the lease even when the same Config is later reused.
 * None of these operations drains consumer events/work. Seal/check/rearm must
 * use the same native interrupt guard to exclude an intervening ISR/Seal.
 * The current WDF backend has no equivalent dispatch-safe lifetime contract
 * and returns UNSUPPORTED; registration and teardown remain available. */
typedef struct {
  CONST VOID *Config;
  CR_INTERRUPT_HANDLER Handler;
  VOID *Context;
  UINT64 Generation,Source;
} CR_INTERRUPT_LEASE;
/* Atomic capability admission + registration + lease publication. Unsupported
 * state-readback backends are rejected before any interrupt can be enabled. */
CR_STATUS CrRegisterInterruptLeased(CR_INTERRUPT_CONFIG *,CR_INTERRUPT_LEASE *);
CR_STATUS CrInterruptGetLease(CR_INTERRUPT_CONFIG *,CR_INTERRUPT_LEASE *);
CR_STATUS CrMaskInterrupt(CONST CR_INTERRUPT_LEASE *);
CR_STATUS CrUnmaskInterrupt(CONST CR_INTERRUPT_LEASE *);
