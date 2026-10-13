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
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "oskinterrupt_wdf.tmh"
#endif

typedef struct {
  CR_INTERRUPT_CONFIG Config;
  EX_RUNDOWN_REF DeferredCallbacks;
  BOOLEAN Closing;
} OSK_WDF_INTERRUPT_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(OSK_WDF_INTERRUPT_CONTEXT,
                                   OskInterruptGetIrqContext);

/* Prevent further ISR callbacks before draining any callback already running
 * at DISPATCH/PASSIVE level. Rundown protection cannot be used at DIRQL. */
STATIC BOOLEAN
OskInterruptSynchronizeClose(IN WDFINTERRUPT Interrupt, IN WDFCONTEXT Context) {
  UNREFERENCED_PARAMETER(Context);
  OSK_WDF_INTERRUPT_CONTEXT *irqCtx = OskInterruptGetIrqContext(Interrupt);
  if (irqCtx == NULL) return FALSE;
  irqCtx->Closing = TRUE;
  return TRUE;
}

BOOLEAN
OskInterruptSynchronizeClear(IN WDFINTERRUPT Interrupt, IN WDFCONTEXT Context) {
  UNREFERENCED_PARAMETER(Context);
  OSK_WDF_INTERRUPT_CONTEXT *irqCtx = OskInterruptGetIrqContext(Interrupt);
  if (irqCtx == NULL) return FALSE;
  irqCtx->Config.Handler = NULL;
  irqCtx->Config.Param = NULL;
  return TRUE;
}

STATIC BOOLEAN
OskInterruptSynchronizeInstall(IN WDFINTERRUPT Interrupt, IN WDFCONTEXT Context) {
  OSK_WDF_INTERRUPT_CONTEXT *irqCtx = OskInterruptGetIrqContext(Interrupt);
  if (irqCtx == NULL) return FALSE;
  irqCtx->Config = *(CR_INTERRUPT_CONFIG *)Context;
  irqCtx->Closing = FALSE;
  return TRUE;
}

STATIC VOID OskInterruptInvokeDeferred(IN WDFINTERRUPT Interrupt) {
  OSK_WDF_INTERRUPT_CONTEXT *irqCtx = OskInterruptGetIrqContext(Interrupt);
  if (irqCtx == NULL || !ExAcquireRundownProtection(&irqCtx->DeferredCallbacks))
    return;
  if (irqCtx->Config.Handler != NULL)
    irqCtx->Config.Handler(irqCtx->Config.Param);
  ExReleaseRundownProtection(&irqCtx->DeferredCallbacks);
}

VOID EvtWdfInterruptDpc(IN WDFINTERRUPT Interrupt,
                        IN WDFOBJECT AssociatedObject) {
  UNREFERENCED_PARAMETER(AssociatedObject);
  TRACE_FUNCTION_ENTRY(TRACE_OSKAL);
  OskInterruptInvokeDeferred(Interrupt);

  TRACE_FUNCTION_EXIT(TRACE_OSKAL);
  return;
}

VOID EvtWdfInterruptWorkitem(IN WDFINTERRUPT Interrupt,
                             IN WDFOBJECT AssociatedObject) {
  UNREFERENCED_PARAMETER(AssociatedObject);
  TRACE_FUNCTION_ENTRY(TRACE_OSKAL);
  OskInterruptInvokeDeferred(Interrupt);
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
  BOOLEAN InterruptRecognized = FALSE;
  log_info("ISR Enter!");

  OSK_WDF_INTERRUPT_CONTEXT *Context = OskInterruptGetIrqContext(Interrupt);
  CR_INTERRUPT_CONFIG *irqContext;
  if (Context == NULL || Context->Closing || Context->Config.Handler == NULL) {
    goto exit;
  }
  irqContext = &Context->Config;

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
    /* FALSE also means the DPC is already queued, not an NTSTATUS error. */
    WdfInterruptQueueDpcForIsr(Interrupt);
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
  if (InterruptConfig!=NULL && InterruptConfig->Backend!=NULL) {
    CONST CR_INTERRUPT_BACKEND *B=InterruptConfig->Backend;
    if (B->Context==NULL || B->Register==NULL || B->Unregister==NULL || B->IsRegistered==NULL)
      return CR_INVALID_PARAMETER;
    return B->Register(B->Context,InterruptConfig);
  }
  // The config must be initialized.
  if (InterruptConfig == NULL ||
      InterruptConfig->Handler == NULL ||
      InterruptConfig->Device == NULL ||
      InterruptConfig->WdfInterruptConfig.InterruptRaw == NULL ||
      InterruptConfig->WdfInterruptConfig.InterruptTranslated == NULL) {
    log_err("Invalid interrupt config");
    return CR_INVALID_PARAMETER;
  }
  if (KeGetCurrentIrql() != PASSIVE_LEVEL) return CR_BUSY;

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
      &InterruptConfig->WdfInterruptAttributes, OSK_WDF_INTERRUPT_CONTEXT);

  /* WDF owns interrupt objects until device removal. Rebind a stopped object
     during the driver's serialized PnP/power path; never delete or recreate
     it on every callback-owner stop. WDF controls D0 activation. */
  if (InterruptConfig->Interrupt != NULL) {
    OSK_WDF_INTERRUPT_CONTEXT *irqCtx =
        OskInterruptGetIrqContext(InterruptConfig->Interrupt);
    if (irqCtx == NULL || irqCtx->Config.Device != InterruptConfig->Device)
      return CR_INVALID_PARAMETER;
    if (irqCtx->Config.Handler != NULL) return CR_ALREADY_STARTED;
    if (irqCtx->Config.WdfInterruptConfig.EvtInterruptIsr !=
            InterruptConfig->WdfInterruptConfig.EvtInterruptIsr ||
        irqCtx->Config.WdfInterruptConfig.EvtInterruptDpc !=
            InterruptConfig->WdfInterruptConfig.EvtInterruptDpc ||
        irqCtx->Config.WdfInterruptConfig.EvtInterruptWorkItem !=
            InterruptConfig->WdfInterruptConfig.EvtInterruptWorkItem ||
        irqCtx->Config.WdfInterruptConfig.PassiveHandling !=
            InterruptConfig->WdfInterruptConfig.PassiveHandling)
      return CR_UNSUPPORTED;
    ExReInitializeRundownProtection(&irqCtx->DeferredCallbacks);
    return WdfInterruptSynchronize(InterruptConfig->Interrupt,
                                  OskInterruptSynchronizeInstall,
                                  InterruptConfig)
               ? CR_SUCCESS : CR_DEVICE_ERROR;
  }

  Status = WdfInterruptCreate(
      InterruptConfig->Device, &InterruptConfig->WdfInterruptConfig,
      &InterruptConfig->WdfInterruptAttributes, &InterruptConfig->Interrupt);
  if (CR_ERROR(Status)) {
    log_err("WdfInterruptCreate failed: %!STATUS!", Status);
    return Status;
  }

  // Populate the interrupt object's context so callbacks can access handler
  OSK_WDF_INTERRUPT_CONTEXT *irqCtx =
      OskInterruptGetIrqContext(InterruptConfig->Interrupt);
  if (irqCtx == NULL) return CR_DEVICE_ERROR;
  ExInitializeRundownProtection(&irqCtx->DeferredCallbacks);
  irqCtx->Config = *InterruptConfig;
  irqCtx->Closing = FALSE;

  return Status;
}

/* Removal is synchronous: callers may release the callback owner only after
 * success. Deferring deletion while reporting success left live callbacks
 * and could lose the interrupt handle on worker allocation failures. */
CR_STATUS
CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig) {
  OSK_WDF_INTERRUPT_CONTEXT *irqCtx;
  if (InterruptConfig == NULL) return CR_INVALID_PARAMETER;
  if (InterruptConfig->Backend!=NULL) {
    CONST CR_INTERRUPT_BACKEND *B=InterruptConfig->Backend;
    return B->Context!=NULL && B->Unregister!=NULL ? B->Unregister(B->Context,InterruptConfig) : CR_INVALID_PARAMETER;
  }
  if (InterruptConfig->Interrupt == NULL) return CR_SUCCESS;
  irqCtx = OskInterruptGetIrqContext(InterruptConfig->Interrupt);
  if (irqCtx == NULL) return CR_DEVICE_ERROR;
  if (irqCtx->Config.Handler == NULL) return CR_SUCCESS;
  if (irqCtx->Config.Handler != InterruptConfig->Handler ||
      irqCtx->Config.Param != InterruptConfig->Param) return CR_NOT_FOUND;
  if (KeGetCurrentIrql() != PASSIVE_LEVEL) return CR_BUSY;
  WdfInterruptDisable(InterruptConfig->Interrupt);
  if (!WdfInterruptSynchronize(InterruptConfig->Interrupt,
                              OskInterruptSynchronizeClose, NULL))
    return CR_DEVICE_ERROR;
  ExWaitForRundownProtectionRelease(&irqCtx->DeferredCallbacks);
  if (!WdfInterruptSynchronize(InterruptConfig->Interrupt,
                              OskInterruptSynchronizeClear, NULL))
    return CR_DEVICE_ERROR;
  /* WdfObjectDelete is invalid for WDFINTERRUPT. Keep the stopped framework
     handle for rebind; handler/context are detached and no callback is live. */
  return CR_SUCCESS;
}

BOOLEAN
CrInterruptIsRegistered(IN CR_INTERRUPT_CONFIG *InterruptConfig) {
  OSK_WDF_INTERRUPT_CONTEXT *Context;
  if (InterruptConfig!=NULL && InterruptConfig->Backend!=NULL) {
    CONST CR_INTERRUPT_BACKEND *B=InterruptConfig->Backend;
    return B->Context!=NULL && B->IsRegistered!=NULL && B->IsRegistered(B->Context,InterruptConfig);
  }
  if (InterruptConfig == NULL || InterruptConfig->Handler == NULL ||
      InterruptConfig->Interrupt == NULL) return FALSE;
  Context = OskInterruptGetIrqContext(InterruptConfig->Interrupt);
  return Context != NULL && Context->Config.Handler == InterruptConfig->Handler &&
         Context->Config.Param == InterruptConfig->Param;
}

/* WDF interrupt enable/disable requires its framework power/IRQL protocol;
 * no dispatch-safe exact-incarnation adapter has been qualified here. */
CR_STATUS CrInterruptGetLease(CR_INTERRUPT_CONFIG *Config,CR_INTERRUPT_LEASE *Out) {
  return Config == NULL || Out == NULL ? CR_INVALID_PARAMETER : CR_UNSUPPORTED;
}
CR_STATUS CrMaskInterrupt(CONST CR_INTERRUPT_LEASE *Lease) {
  return Lease == NULL ? CR_INVALID_PARAMETER : CR_UNSUPPORTED;
}
CR_STATUS CrUnmaskInterrupt(CONST CR_INTERRUPT_LEASE *Lease) {
  return Lease == NULL ? CR_INVALID_PARAMETER : CR_UNSUPPORTED;
}

CR_STATUS CrRegisterInterruptLeased(CR_INTERRUPT_CONFIG *Config,CR_INTERRUPT_LEASE *Out) {
  return Config == NULL || Out == NULL ? CR_INVALID_PARAMETER : CR_UNSUPPORTED;
}
