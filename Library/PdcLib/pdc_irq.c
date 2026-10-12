/* Native convenience routes retain exact ownership through failed cleanup.
 * SPDX-License-Identifier: MIT */
#include "pdc_internal.h"

#ifndef _KERNEL_MODE
typedef struct {
  PdcDeviceContext *Context;
  UINT16 Pin;
  CR_INTERRUPT_TRIGGER_TYPE Type;
  CR_INTERRUPT_CONFIG Interrupt;
  CR_INTERRUPT_HANDLER Handler;
  VOID *Param;
  PDC_INTERRUPT_REGISTERS Registers;
  UINT32 OldType,OldEnable,OldSpi;
  BOOLEAN Changed,Ready;
} PDC_INTERRUPT_OWNER;
STATIC PDC_INTERRUPT_OWNER mPdcOwners[MAX_INTERRUPT_ENTRIES];

STATIC VOID Dispatch(VOID *Parameter) {
  PDC_INTERRUPT_OWNER *Owner=Parameter;
  if (Owner->Ready) Owner->Handler(Owner->Param);
}
STATIC VOID Restore(PDC_INTERRUPT_OWNER *Owner) {
  if (!Owner->Changed) return;
  PDC_INTERRUPT_REGISTERS *R=&Owner->Registers;
  UINT32 Value=CrMmioRead32(R->ConfigAddress);
  CrMmioWrite32(R->ConfigAddress,(Value&~7U)|Owner->OldType);
  if (R->SpiMask!=0) {
    Value=CrMmioRead32(R->SpiAddress);
    CrMmioWrite32(R->SpiAddress,(Value&~R->SpiMask)|Owner->OldSpi);
  }
  Value=CrMmioRead32(R->EnableAddress);
  CrMmioWrite32(R->EnableAddress,(Value&~R->EnableMask)|Owner->OldEnable);
}
STATIC VOID Retire(PDC_INTERRUPT_OWNER *Owner) {
  --Owner->Context->ActiveInterrupts;
  *Owner=(PDC_INTERRUPT_OWNER){0};
}

CR_STATUS PdcRegisterInterrupt(PdcDeviceContext *Ctx,UINT16 Pin,
    CR_INTERRUPT_HANDLER Handler,VOID *Parameter,CR_INTERRUPT_TRIGGER_TYPE Type) {
  if (Handler==NULL) return CR_INVALID_PARAMETER;
  PDC_INTERRUPT_REGISTERS Registers;
  CR_STATUS Status=PdcValidateInterrupt(Ctx,Pin,Type,&Registers);
  if (CR_ERROR(Status)) return Status;
  UINT16 Gic;
  Status=PdcGetGicIrqFromPdcPin(Ctx,Pin,&Gic);
  if (CR_ERROR(Status)) return Status;
  CR_INTERRUPT_GUARD Guard;
  Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  PDC_INTERRUPT_OWNER *Owner=NULL;
  for (UINTN I=0;I<MAX_INTERRUPT_ENTRIES;++I) {
    if (mPdcOwners[I].Context!=NULL &&
        (mPdcOwners[I].Interrupt.InterruptNumber==Gic ||
         (mPdcOwners[I].Context==Ctx && mPdcOwners[I].Pin==Pin))) {
      Status=CR_BUSY;goto Exit;
    }
    if (mPdcOwners[I].Context==NULL && Owner==NULL) Owner=&mPdcOwners[I];
  }
  if (Owner==NULL) {Status=CR_OUT_OF_RESOURCES;goto Exit;}
  *Owner=(PDC_INTERRUPT_OWNER){.Context=Ctx,.Pin=Pin,.Type=Type,
      .Handler=Handler,.Param=Parameter,.Registers=Registers,
      .Interrupt={.InterruptNumber=Gic,.Handler=Dispatch,.Param=Owner,
                  .TriggerType=Registers.GicType}};
  ++Ctx->ActiveInterrupts;
  // Claim GIC ownership before touching PDC, so a foreign owner is untouched.
  // HIGH priority excludes native dispatch until the complete route is ready.
  Status=CrRegisterInterrupt(&Owner->Interrupt);
  if (CR_ERROR(Status)) {
    if (!CrInterruptIsRegistered(&Owner->Interrupt)) Retire(Owner);
    // A retained callback is gated off and can only be retired by this owner.
    goto Exit;
  }
  Owner->OldType=CrMmioRead32(Registers.ConfigAddress)&7U;
  Owner->OldEnable=CrMmioRead32(Registers.EnableAddress)&Registers.EnableMask;
  if (Registers.SpiMask!=0)
    Owner->OldSpi=CrMmioRead32(Registers.SpiAddress)&Registers.SpiMask;
  Owner->Changed=TRUE;
  Status=PdcEnableInterrupt(Ctx,Pin,FALSE);
  if (!CR_ERROR(Status)) Status=PdcGicSetType(Ctx,Pin,Type,NULL);
  if (!CR_ERROR(Status)) Status=PdcEnableInterrupt(Ctx,Pin,TRUE);
  if (!CR_ERROR(Status)) {Owner->Ready=TRUE;goto Exit;}
  CR_STATUS Cleanup=CrUnregisterInterrupt(&Owner->Interrupt);
  if (CR_ERROR(Cleanup)) {Status=Cleanup;goto Exit;}
  Restore(Owner);Retire(Owner);
Exit:
  CrInterruptGuardRelease(&Guard);
  return Status;
}

CR_STATUS PdcUnregisterInterrupt(PdcDeviceContext *Ctx,UINT16 Pin,
                                 CR_INTERRUPT_TRIGGER_TYPE Type) {
  if (Ctx==NULL || (UINT32)Type>CR_INTERRUPT_TRIGGER_EDGE_BOTH)
    return CR_INVALID_PARAMETER;
  CR_INTERRUPT_GUARD Guard;
  CR_STATUS Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  PDC_INTERRUPT_OWNER *Owner=NULL;
  for (UINTN I=0;I<MAX_INTERRUPT_ENTRIES;++I)
    if (mPdcOwners[I].Context==Ctx && mPdcOwners[I].Pin==Pin) {Owner=&mPdcOwners[I];break;}
  if (Owner==NULL) {Status=CR_NOT_FOUND;goto Exit;}
  if (Owner->Type!=Type) {Status=CR_INVALID_PARAMETER;goto Exit;}
  if (!CrInterruptIsRegistered(&Owner->Interrupt)) {Status=CR_BUSY;goto Exit;}
  if (Owner->Changed) {
    Status=PdcEnableInterrupt(Ctx,Pin,FALSE);
    if (CR_ERROR(Status)) goto Exit;
  }
  Status=CrUnregisterInterrupt(&Owner->Interrupt);
  if (CR_ERROR(Status)) goto Exit;
  // Synchronous OSKAL retirement precedes release of caller callback storage.
  Owner->Ready=FALSE;
  Restore(Owner);Retire(Owner);
Exit:
  CrInterruptGuardRelease(&Guard);
  return Status;
}
#endif
