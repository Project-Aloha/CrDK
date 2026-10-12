/* SPDX-License-Identifier: GPL-2.0-only */
#include "NativeEvents.h"
#include <Library/UefiBootServicesTableLib.h>
#include <Library/BaseMemoryLib.h>
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Level(VOID){
  if(!gBS||!gBS->RaiseTPL||!gBS->RestoreTPL)return EFI_NOT_READY;
  EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);gBS->RestoreTPL(Old);
  return Old<=TPL_CALLBACK?EFI_SUCCESS:EFI_INVALID_PARAMETER;
}
STATIC BOOLEAN Valid(CONST MDC_NATIVE_EVENTS *E){return E&&E->Self==E&&E->Initialized&&E->Incarnation;}
BOOLEAN MdcEventsOwned(CONST MDC_NATIVE_EVENTS *E){return Valid(E);}
STATIC VOID Irq(VOID *Context)
{
  MDC_NATIVE_EVENTS *E=Context;
  /* Native OSKAL invokes this under HIGH, excludes registration/Seal, and
   * delays lower-priority SignalEvent work until after its GIC EOI. */
  if(!Valid(E)||!E->Registered)return;
  EFI_STATUS S=Exact(CrMaskInterrupt(&E->Lease));
  if(S!=EFI_SUCCESS){E->LastStatus=S;E->Faulted=TRUE;E->Admission=FALSE;}
  if(E->Sealed||E->Queued)return;
  E->Queued=TRUE;
  S=Exact(gBS->SignalEvent(E->Work));
  if(S!=EFI_SUCCESS){E->LastStatus=S;E->Faulted=TRUE;E->Admission=FALSE;}
  /* Ambiguous queue failure retains Queued and all storage. Only an actual
   * callback or independently qualified event cancellation could drain it. */
}
STATIC VOID EFIAPI Work(EFI_EVENT Event,VOID *Context)
{
  MDC_NATIVE_EVENTS *E=Context;CR_INTERRUPT_GUARD G;
  if(!Valid(E)||Event!=E->Work)return;
  EFI_STATUS S=Exact(CrInterruptGuardAcquire(&G));
  if(S!=EFI_SUCCESS){E->LastStatus=S;return;}
  if(!E->Queued||E->Running){CrInterruptGuardRelease(&G);return;}
  E->Queued=FALSE;E->Running=TRUE;
  UINT64 Generation=E->Incarnation;
  BOOLEAN Service=E->Admission&&!E->Sealed&&!E->Faulted;
  CrInterruptGuardRelease(&G);
  S=Service?Exact(E->Service(E->Context,Generation)):EFI_SUCCESS;
  EFI_STATUS GuardStatus=Exact(CrInterruptGuardAcquire(&G));
  if(GuardStatus!=EFI_SUCCESS){E->LastStatus=GuardStatus;return;}
  E->Running=FALSE;
  if(S!=EFI_SUCCESS){E->LastStatus=S;E->Faulted=TRUE;E->Admission=FALSE;}
  if(E->Incarnation!=Generation){E->LastStatus=EFI_ACCESS_DENIED;E->Faulted=TRUE;E->Admission=FALSE;}
  /* The same HIGH guard covers Seal/identity validation AND actual unmask;
   * no check-then-rearm window lets an old worker reopen a sealed source. */
  if(E->Registered&&E->Admission&&!E->Sealed&&!E->Faulted){
    S=Exact(CrUnmaskInterrupt(&E->Lease));
    if(S!=EFI_SUCCESS){E->LastStatus=S;E->Faulted=TRUE;E->Admission=FALSE;}
  }
  CrInterruptGuardRelease(&G);
}
EFI_STATUS MdcEventsOpen(MDC_NATIVE_EVENTS *E,VOID *Context,UINT64 Incarnation,
                        EFI_STATUS (*Service)(VOID *,UINT64))
{
  CR_INTERRUPT_GUARD G;EFI_STATUS S;
  if(!E||!Incarnation||!Service||!gBS||!gBS->CreateEvent||!gBS->SignalEvent||!gBS->CloseEvent)
    return EFI_INVALID_PARAMETER;
  S=Level();if(S!=EFI_SUCCESS)return S;
  if(E->Initialized||E->Self||E->Work)return EFI_ALREADY_STARTED;
  E->Self=E;E->Context=Context;E->Incarnation=Incarnation;E->Service=Service;
  E->Initialized=TRUE;E->LastStatus=EFI_NOT_READY;
  S=Exact(gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Work,E,&E->Work));
  if(S!=EFI_SUCCESS){E->LastStatus=S;return S;}
  E->Config.Param=E;E->Config.Handler=Irq;E->Config.InterruptNumber=83+32;
  E->Config.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH;
  S=Exact(CrInterruptGuardAcquire(&G));
  if(S!=EFI_SUCCESS){E->LastStatus=S;return S;}
  /* Get the generation while dispatch is excluded, including MU's immediate
   * source-enable behavior during registration. No ISR sees an empty lease. */
  S=Exact(CrRegisterInterruptLeased(&E->Config,&E->Lease));
  E->Registered=CrInterruptIsRegistered(&E->Config);
  if(S==EFI_SUCCESS&&!E->Registered)S=EFI_NOT_READY;
  E->Admission=S==EFI_SUCCESS;E->LastStatus=S;
  CrInterruptGuardRelease(&G);return S;
}
EFI_STATUS MdcEventsSeal(MDC_NATIVE_EVENTS *E)
{
  CR_INTERRUPT_GUARD G;EFI_STATUS S;
  if(!E)return EFI_INVALID_PARAMETER;
  S=Level();if(S!=EFI_SUCCESS)return S;
  if(!E->Initialized)return E->Self||E->Work?EFI_INVALID_PARAMETER:EFI_SUCCESS;
  if(!Valid(E))return EFI_INVALID_PARAMETER;
  S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS)return S;
  E->Sealed=TRUE;E->Admission=FALSE;
  if(E->Registered){
    /* Exact config/context unregister masks and removes the ISR. Its own
     * checked rollback keeps registration on any uncertain backend result. */
    S=Exact(CrUnregisterInterrupt(&E->Config));
    E->Registered=CrInterruptIsRegistered(&E->Config);
    if(S!=EFI_SUCCESS||E->Registered){
      if(S==EFI_SUCCESS)S=EFI_NOT_READY;
      E->LastStatus=S;CrInterruptGuardRelease(&G);return S;
    }
  }
  BOOLEAN Pending=E->Queued||E->Running;
  CrInterruptGuardRelease(&G);
  if(Pending)return EFI_NOT_READY;
  if(E->Work){S=Exact(gBS->CloseEvent(E->Work));if(S!=EFI_SUCCESS){E->LastStatus=S;return S;}}
  ZeroMem(E,sizeof(*E));return EFI_SUCCESS;
}
