/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include <oskal/cr_assert.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <wdm.h>
#include "oskinterrupt_wdf.tmh"

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CR_INTERRUPT_CONFIG,
                                   OskInterruptGetIrqContext);

// Synchronize routine used to clear handler/context while holding the
// interrupt lock so ISR cannot race with unregistration.
BOOLEAN
OskInterruptSynchronizeClear(IN WDFINTERRUPT Interrupt, IN WDFCONTEXT Context) {
  UNREFERENCED_PARAMETER(Context);
  CR_INTERRUPT_CONFIG *irqCtx = OskInterruptGetIrqContext(Interrupt);
  if (irqCtx != NULL) {
    irqCtx->Handler = NULL;
    irqCtx->Param = NULL;
  }
  return TRUE;
}

VOID EvtWdfInterruptDpc(IN WDFINTERRUPT Interrupt,
                        IN WDFOBJECT AssociatedObject) {
  UNREFERENCED_PARAMETER(AssociatedObject);
  CR_INTERRUPT_CONFIG *irqContext = OskInterruptGetIrqContext(Interrupt);
  if (irqContext == NULL || irqContext->Handler == NULL) {
    return;
  }
  TRACE_FUNCTION_ENTRY(TRACE_OSKAL);
  log_info("DPC Enter!");
  WdfInterruptAcquireLock(Interrupt);
  irqContext->Handler(irqContext->Param);
  WdfInterruptReleaseLock(Interrupt);

  TRACE_FUNCTION_EXIT(TRACE_OSKAL);
  return;
}

VOID EvtWdfInterruptWorkitem(IN WDFINTERRUPT Interrupt,
                             IN WDFOBJECT AssociatedObject) {
  UNREFERENCED_PARAMETER(AssociatedObject);
  TRACE_FUNCTION_ENTRY(TRACE_OSKAL);
  CR_INTERRUPT_CONFIG *irqContext = OskInterruptGetIrqContext(Interrupt);
  if (irqContext == NULL || irqContext->Handler == NULL) {
    return;
  }
  irqContext->Handler(irqContext->Param);
  TRACE_FUNCTION_EXIT(TRACE_OSKAL);
  return;
};

// Generic Isr for passing correct params to lib.
BOOLEAN
CrInternalInterruptEntry(IN WDFINTERRUPT Interrupt, // WDF interrupt handle
                         IN ULONG MessageID         // For msi interrupt
) {
  UNREFERENCED_PARAMETER(MessageID);
  // Get context from interrupt
  CR_STATUS Status = CR_SUCCESS;
  BOOLEAN InterruptRecognized = FALSE;
  log_info("ISR Enter!");

  CR_INTERRUPT_CONFIG *irqContext = OskInterruptGetIrqContext(Interrupt);
  if (irqContext == NULL || irqContext->Handler == NULL) {
    goto exit;
  }

  // Call Recognize function if provided
  {
    // TODO
    InterruptRecognized = TRUE;
  }

  // No DPC or workitem provided, call handler directly in isr
  if (irqContext->WdfInterruptConfig.EvtInterruptDpc == NULL &&
      irqContext->WdfInterruptConfig.EvtInterruptWorkItem == NULL) {
    irqContext->Handler(irqContext->Param);
    goto exit;
  }

  // Queue DPC or workitem
  if (NULL != irqContext->WdfInterruptConfig.EvtInterruptDpc) {
    if (irqContext->WdfInterruptConfig.PassiveHandling) {
      // log an error if someone is requesting dpc passive handling
      log_err("Interrupt DPC queue in passive handling is not suggested");
    }

    // Queue a DPC if not passive handling
    Status = WdfInterruptQueueDpcForIsr(Interrupt);
    if (CR_ERROR(Status)) {
      log_err("WdfInterruptQueueDpcForIsr failed: %!STATUS!", Status);
      goto exit;
    }
    goto exit;
  }

  // Always queue workitem if provided
  if (irqContext->WdfInterruptConfig.EvtInterruptWorkItem != NULL) {
    // Passive handling, queue workitem
    WdfInterruptQueueWorkItemForIsr(Interrupt);
  }

exit:
  return InterruptRecognized;
}

CR_STATUS
CrRegisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig) {
  CR_STATUS Status = CR_SUCCESS;
  // The config must be initialized.
  if (InterruptConfig == NULL ||
      InterruptConfig->WdfInterruptConfig.InterruptRaw == NULL ||
      InterruptConfig->WdfInterruptConfig.InterruptTranslated == NULL) {
    log_err("Invalid interrupt config");
    return CR_INVALID_PARAMETER;
  }

  // Set the Isr
  if (InterruptConfig->WdfInterruptConfig.EvtInterruptIsr == NULL) {
    InterruptConfig->WdfInterruptConfig.EvtInterruptIsr =
        CrInternalInterruptEntry;
  }
  if ((InterruptConfig->LIrqLCbType == OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_DPC) &&
      !InterruptConfig->WdfInterruptConfig.PassiveHandling &&
      InterruptConfig->WdfInterruptConfig.EvtInterruptDpc == NULL) {
    // ensure we set the actual DPC callback implemented in this file
    InterruptConfig->WdfInterruptConfig.EvtInterruptDpc = EvtWdfInterruptDpc;
  }
  if ((InterruptConfig->LIrqLCbType ==
       OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_WORKITEM) &&
      InterruptConfig->WdfInterruptConfig.EvtInterruptWorkItem == NULL) {
    InterruptConfig->WdfInterruptConfig.EvtInterruptWorkItem =
        EvtWdfInterruptWorkitem;
  }

  // Ensure the interrupt object's context type is set in attributes.
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(
      &InterruptConfig->WdfInterruptAttributes, CR_INTERRUPT_CONFIG);

  Status = WdfInterruptCreate(
      InterruptConfig->Device, &InterruptConfig->WdfInterruptConfig,
      &InterruptConfig->WdfInterruptAttributes, &InterruptConfig->Interrupt);
  if (CR_ERROR(Status)) {
    log_err("WdfInterruptCreate failed: %!STATUS!", Status);
    return Status;
  }

  // Populate the interrupt object's context so callbacks can access handler
  CR_INTERRUPT_CONFIG *irqCtx =
      OskInterruptGetIrqContext(InterruptConfig->Interrupt);
  if (irqCtx != NULL) {
    irqCtx->Handler = InterruptConfig->Handler;
    irqCtx->Param = InterruptConfig->Param;
    irqCtx->WdfInterruptConfig = InterruptConfig->WdfInterruptConfig;
    irqCtx->WdfInterruptAttributes = InterruptConfig->WdfInterruptAttributes;
    irqCtx->Interrupt = InterruptConfig->Interrupt;
    irqCtx->Device = InterruptConfig->Device;
  }

  return Status;
}

// helper context for queued IO work item
typedef struct _CR_UNREGISTER_WORK_CTX {
  PIO_WORKITEM WorkItem;
  CR_INTERRUPT_CONFIG *InterruptConfig;
  WDFINTERRUPT SavedInterrupt;
} CR_UNREGISTER_WORK_CTX, *PCR_UNREGISTER_WORK_CTX;

// IO workitem callback runs at PASSIVE_LEVEL; free workitem & context here.
static VOID
CrUnregisterInterruptWorkerIo(PDEVICE_OBJECT DeviceObject, PVOID Context) {
  UNREFERENCED_PARAMETER(DeviceObject);
  PCR_UNREGISTER_WORK_CTX ctx = (PCR_UNREGISTER_WORK_CTX)Context;
  PIO_WORKITEM workItem = NULL;
  WDFINTERRUPT savedInterrupt = NULL;

  if (ctx == NULL) {
    return;
  }

  workItem = ctx->WorkItem;
  savedInterrupt = ctx->SavedInterrupt;

  if (savedInterrupt != NULL) {
    WdfInterruptDisable(savedInterrupt);
    WdfObjectDelete(savedInterrupt);
  }

  if (workItem != NULL) {
    IoFreeWorkItem(workItem);
  }

  ExFreePoolWithTag(ctx, 'rUnI');
}

CR_STATUS
CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig) {
  if (InterruptConfig == NULL) {
    log_err("CrUnregisterInterrupt: NULL parameter");
    return CR_INVALID_PARAMETER;
  }

  if (InterruptConfig->Interrupt == NULL) {
    return CR_SUCCESS;
  }

  // If we are not at PASSIVE_LEVEL, defer the deletion to a worker so that
  // WDF internals that require PASSIVE_LEVEL are executed safely.
  if (KeGetCurrentIrql() > PASSIVE_LEVEL) {
    PCR_UNREGISTER_WORK_CTX ctx =
        (PCR_UNREGISTER_WORK_CTX)ExAllocatePoolWithTag(NonPagedPoolNx,
                                                       sizeof(*ctx), 'rUnI');
    if (ctx == NULL) {
      log_err("CrUnregisterInterrupt: failed to allocate work context");
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(ctx, sizeof(*ctx));
    // Atomically take ownership of the interrupt handle so other threads
    // won't race to use/delete it while we defer deletion.
    WDFINTERRUPT saved = (WDFINTERRUPT)InterlockedExchangePointer(
        (PVOID *)&InterruptConfig->Interrupt, NULL);
    if (saved == NULL) {
      // Already cleared by someone else.
      ExFreePoolWithTag(ctx, 'rUnI');
      return CR_SUCCESS;
    }
    ctx->InterruptConfig = InterruptConfig;
    ctx->SavedInterrupt = saved;

    // Allocate IO workitem associated with the device so the queued routine
    // will execute at PASSIVE_LEVEL. Use WDF helper to get the underlying
    // PDEVICE_OBJECT for the WDF device stored in InterruptConfig.
    PDEVICE_OBJECT devObj = WdfDeviceWdmGetDeviceObject(InterruptConfig->Device);
    if (devObj == NULL) {
      ExFreePoolWithTag(ctx, 'rUnI');
      log_err("CrUnregisterInterrupt: cannot get PDEVICE_OBJECT");
      return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->WorkItem = IoAllocateWorkItem(devObj);
    if (ctx->WorkItem == NULL) {
      ExFreePoolWithTag(ctx, 'rUnI');
      log_err("CrUnregisterInterrupt: IoAllocateWorkItem failed");
      return STATUS_INSUFFICIENT_RESOURCES;
    }

    IoQueueWorkItem(ctx->WorkItem, CrUnregisterInterruptWorkerIo,
                    DelayedWorkQueue, ctx);
    return CR_SUCCESS;
  }

  // PASSIVE_LEVEL: safe to perform synchronous deletion.
  WdfInterruptDisable(InterruptConfig->Interrupt);
  WdfObjectDelete(InterruptConfig->Interrupt);
  InterruptConfig->Interrupt = NULL;

  return CR_SUCCESS;
}