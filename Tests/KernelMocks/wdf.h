/* Host-only WDF API shape; no manual interrupt deletion API is provided.
   SPDX-License-Identifier: MIT */
#pragma once
#include <ntddk.h>
typedef struct WDF_TEST_INTERRUPT *WDFINTERRUPT;
typedef VOID *WDFDEVICE, *WDFCONTEXT, *WDFOBJECT, *WDFWORKITEM;
typedef struct {
  VOID *InterruptRaw, *InterruptTranslated;
  BOOLEAN (*EvtInterruptIsr)(WDFINTERRUPT, ULONG);
  VOID (*EvtInterruptDpc)(WDFINTERRUPT, WDFOBJECT);
  VOID (*EvtInterruptWorkItem)(WDFINTERRUPT, WDFOBJECT);
  BOOLEAN PassiveHandling;
} WDF_INTERRUPT_CONFIG;
typedef struct { SIZE_T ContextSize; } WDF_OBJECT_ATTRIBUTES;
#define WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(attrs,type) \
  ((attrs)->ContextSize = sizeof(type))
VOID *WdfTestGetContext(WDFINTERRUPT interrupt);
#define WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(type,name) \
  static inline type *name(WDFINTERRUPT interrupt) { return WdfTestGetContext(interrupt); }
NTSTATUS WdfInterruptCreate(WDFDEVICE device, WDF_INTERRUPT_CONFIG *config,
                           WDF_OBJECT_ATTRIBUTES *attributes, WDFINTERRUPT *interrupt);
VOID WdfInterruptDisable(WDFINTERRUPT interrupt);
BOOLEAN WdfInterruptSynchronize(WDFINTERRUPT interrupt,
                                BOOLEAN (*callback)(WDFINTERRUPT, WDFCONTEXT), WDFCONTEXT context);
BOOLEAN WdfInterruptQueueDpcForIsr(WDFINTERRUPT interrupt);
BOOLEAN WdfInterruptQueueWorkItemForIsr(WDFINTERRUPT interrupt);
