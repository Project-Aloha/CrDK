/* HDK8245 native deferred IRQ lifetime, independent of command transport.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>
#include <oskal/cr_interrupt.h>
typedef struct _MDC_NATIVE_EVENTS {
  struct _MDC_NATIVE_EVENTS *Self;
  CR_INTERRUPT_CONFIG Config;
  CR_INTERRUPT_LEASE Lease;
  EFI_EVENT Work;
  VOID *Context;
  /* Called at TPL_CALLBACK, outside the IRQ guard. Native implementation
   * enters the exact clock/Operations token, validates this incarnation,
   * invokes the actual command IRQ core and ends that same operation. */
  EFI_STATUS (*Service)(VOID *Context,UINT64 Incarnation);
  UINT64 Incarnation;
  EFI_STATUS LastStatus;
  BOOLEAN Initialized,Registered,Admission,Queued,Running,Sealed,Faulted;
} MDC_NATIVE_EVENTS;
/* Owner storage is retained through successful Seal; initialization requires
 * zeroed storage and monotonic native incarnation. No timer polling. */
EFI_STATUS MdcEventsOpen(MDC_NATIVE_EVENTS *,VOID *Context,UINT64 Incarnation,
                        EFI_STATUS (*Service)(VOID *,UINT64));
/* Seal/source removal is not work completion. Pending/running notification
 * returns NOT_READY; caller releases Operations so it can finish, then retries.
 * Success includes real unregister, drained queued/running work and CloseEvent. */
EFI_STATUS MdcEventsSeal(MDC_NATIVE_EVENTS *);
BOOLEAN MdcEventsOwned(CONST MDC_NATIVE_EVENTS *);
