/* Native IRQ convenience adapter. Windows GPIO uses its class extension.
 * SPDX-License-Identifier: MIT */
#include "gpio_internal.h"
#include <oskal/cr_memory.h>

#ifndef _KERNEL_MODE
typedef struct {
  GpioDeviceContext *Context;
  PdcDeviceContext *Pdc;
  UINT16 Pin;
  CR_INTERRUPT_TRIGGER_TYPE Type;
  CR_INTERRUPT_HANDLER Handler;
  VOID *Param;
  UINT32 OldConfig,OldTarget;
} GPIO_INTERRUPT_OWNER;
STATIC GPIO_INTERRUPT_OWNER mGpioOwners[GPIO_PINS_MAX];

STATIC CR_STATUS ValidatePin(GpioDeviceContext *Ctx,UINT16 Pin,
                             CR_INTERRUPT_TRIGGER_TYPE Type) {
  if (Ctx==NULL || Ctx->PinCount>GPIO_PINS_MAX || Pin>=Ctx->PinCount ||
      Ctx->GpioPins[Pin].pRegCfg==NULL || (UINT32)Type>CR_INTERRUPT_TRIGGER_EDGE_BOTH)
    return CR_INVALID_PARAMETER;
  GpioPinInfo *P=&Ctx->GpioPins[Pin];GpioConfigRegInfo *R=P->pRegCfg;
  if (P->Reserved || !R->InterruptEnMsk || !R->InterruptDetectionMsk ||
      !R->InterruptRawStatusMsk || !R->InterruptStatusMsk ||
      (R->InterruptDetectionWidth!=1 && R->InterruptDetectionWidth!=2))
    return CR_UNSUPPORTED;
  if (P->PdcPinNumber==GPIO_PDC_PIN_NUMBER_INVALID &&
      R->InterruptDetectionWidth==1 && Type==CR_INTERRUPT_TRIGGER_EDGE_BOTH)
    return CR_UNSUPPORTED;
  UINT32 Offsets[]={R->IntCfgRegOffset,R->IntStatusRegOffset,R->IntTargetRegOffset};
  for (UINTN I=0;I<ARRAY_SIZE(Offsets);++I)
    if (P->PinAddress==0 || (P->PinAddress&3)!=0 || (Offsets[I]&3)!=0 ||
        P->PinAddress>(UINTN)-1-Offsets[I] ||
        P->PinAddress+Offsets[I]>(UINTN)-1-3) return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
STATIC VOID Restore(GPIO_INTERRUPT_OWNER *Owner) {
  GpioConfigRegInfo *R=Owner->Context->GpioPins[Owner->Pin].pRegCfg;
  UINT32 Mask=R->InterruptEnMsk|R->InterruptRawStatusMsk|
              R->InterruptDetectionMsk|R->InterruptPolarityMsk;
  UINTN Address=GpioGetCfgRegByIndex(Owner->Context,GPIO_CFG_REG_TYPE_INT_CFG_REG,Owner->Pin);
  UINT32 Value=CrMmioRead32(Address);
  CrMmioWrite32(Address,(Value&~Mask)|(Owner->OldConfig&Mask));
  Address=GpioGetCfgRegByIndex(Owner->Context,GPIO_CFG_REG_TYPE_INT_TARGET_REG,Owner->Pin);
  Value=CrMmioRead32(Address);Mask=R->InterruptTargetMsk;
  CrMmioWrite32(Address,(Value&~Mask)|(Owner->OldTarget&Mask));
}
VOID GpioIsr(VOID *Parameter) {
  GpioDeviceContext *Ctx=Parameter;
  if (Ctx==NULL || Ctx->PinCount>GPIO_PINS_MAX) return;
  // Holes and other controller entries must not hide later registrations.
  for (UINTN I=0;I<GPIO_PINS_MAX;++I) {
    GPIO_INTERRUPT_OWNER *Owner=&mGpioOwners[I];
    if (Owner->Context!=Ctx || Owner->Pdc!=NULL || Owner->Handler==NULL) continue;
    if (GpioReadIrqStatus(Ctx,Owner->Pin)) {
      Owner->Handler(Owner->Param);GpioCleanIrqStatus(Ctx,Owner->Pin);
    }
  }
}
CR_STATUS GpioRegisterGpioIrq(GpioDeviceContext *Ctx,PdcDeviceContext *Pdc,
    UINT16 Pin,CR_INTERRUPT_HANDLER Handler,VOID *Param,CR_INTERRUPT_TRIGGER_TYPE Type) {
  if (Handler==NULL) return CR_INVALID_PARAMETER;
  CR_STATUS Status=ValidatePin(Ctx,Pin,Type);
  if (CR_ERROR(Status)) return Status;
  CR_INTERRUPT_GUARD Guard;
  Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  GPIO_INTERRUPT_OWNER *Owner=NULL;
  for (UINTN I=0;I<GPIO_PINS_MAX;++I) {
    if (mGpioOwners[I].Context==Ctx && mGpioOwners[I].Pin==Pin) {Status=CR_BUSY;goto Exit;}
    if (mGpioOwners[I].Context==NULL && Owner==NULL) Owner=&mGpioOwners[I];
  }
  if (Owner==NULL) {Status=CR_OUT_OF_RESOURCES;goto Exit;}
  if (Ctx->GpioPins[Pin].PdcPinNumber!=GPIO_PDC_PIN_NUMBER_INVALID) {
    if (Pdc==NULL) {Status=CR_INVALID_PARAMETER;goto Exit;}
    UINT16 Before=Pdc->ActiveInterrupts;
    Status=PdcRegisterInterrupt(Pdc,Ctx->GpioPins[Pin].PdcPinNumber,Handler,Param,Type);
    if (CR_ERROR(Status) && Pdc->ActiveInterrupts==Before) goto Exit;
    // Keep the route owner on failed backend cleanup for explicit retry.
    *Owner=(GPIO_INTERRUPT_OWNER){.Context=Ctx,.Pdc=Pdc,.Pin=Pin,.Type=Type,
                                  .Handler=Handler,.Param=Param};
    ++Ctx->ActiveInterrupts;goto Exit;
  }
  if (!CrInterruptIsRegistered(&Ctx->InterruptConfig)) {Status=CR_NOT_READY;goto Exit;}
  *Owner=(GPIO_INTERRUPT_OWNER){.Context=Ctx,.Pin=Pin,.Type=Type,.Handler=Handler,.Param=Param};
  Owner->OldConfig=CrMmioRead32(GpioGetCfgRegByIndex(Ctx,GPIO_CFG_REG_TYPE_INT_CFG_REG,Pin));
  Owner->OldTarget=CrMmioRead32(GpioGetCfgRegByIndex(Ctx,GPIO_CFG_REG_TYPE_INT_TARGET_REG,Pin));
  Status=GpioSetInterruptCfg(Ctx,Pin,Type);
  if (!CR_ERROR(Status)) Status=GpioEnableInterrupt(Ctx,Pin,TRUE);
  if (CR_ERROR(Status)) {Restore(Owner);*Owner=(GPIO_INTERRUPT_OWNER){0};}
  else ++Ctx->ActiveInterrupts;
Exit:
  CrInterruptGuardRelease(&Guard);
  return Status;
}
CR_STATUS GpioUnregisterGpioIrq(GpioDeviceContext *Ctx,UINT16 Pin,CR_INTERRUPT_TRIGGER_TYPE Type) {
  CR_STATUS Status=ValidatePin(Ctx,Pin,Type);
  if (CR_ERROR(Status)) return Status;
  CR_INTERRUPT_GUARD Guard;
  Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  GPIO_INTERRUPT_OWNER *Owner=NULL;
  for (UINTN I=0;I<GPIO_PINS_MAX;++I)
    if (mGpioOwners[I].Context==Ctx && mGpioOwners[I].Pin==Pin) {Owner=&mGpioOwners[I];break;}
  if (Owner==NULL) {Status=CR_NOT_FOUND;goto Exit;}
  if (Owner->Type!=Type) {Status=CR_INVALID_PARAMETER;goto Exit;}
  if (Owner->Pdc!=NULL)
    Status=PdcUnregisterInterrupt(Owner->Pdc,Ctx->GpioPins[Pin].PdcPinNumber,Type);
  else {
    Status=GpioEnableInterrupt(Ctx,Pin,FALSE);
    if (!CR_ERROR(Status)) Restore(Owner);
  }
  if (CR_ERROR(Status)) goto Exit;
  *Owner=(GPIO_INTERRUPT_OWNER){0};--Ctx->ActiveInterrupts;
Exit:
  CrInterruptGuardRelease(&Guard);
  return Status;
}
CR_STATUS GpioInitIrq(GpioDeviceContext *Ctx) {
  if (Ctx==NULL || Ctx->PinCount==0 || Ctx->PinCount>GPIO_PINS_MAX) return CR_INVALID_PARAMETER;
  CR_INTERRUPT_GUARD Guard;
  CR_STATUS Status=CrInterruptGuardAcquire(&Guard);
  if (CR_ERROR(Status)) return Status;
  /* OSKAL retains this exact address until successful unregister. Never
   * register a temporary then copy it into the controller context. */
  if (CrInterruptIsRegistered(&Ctx->InterruptConfig)) {
    CrInterruptGuardRelease(&Guard);return CR_BUSY;
  }
  CR_INTERRUPT_CONFIG Saved=Ctx->InterruptConfig;
  Ctx->InterruptConfig.Handler=GpioIsr;Ctx->InterruptConfig.Param=Ctx;
  Status=CrRegisterInterrupt(&Ctx->InterruptConfig);
  /* A failed backend rollback may still own the callback. Keep its stable
   * configuration intact so the caller can explicitly retire it later. */
  if (Status!=CR_SUCCESS && !CrInterruptIsRegistered(&Ctx->InterruptConfig))
    Ctx->InterruptConfig=Saved;
  CrInterruptGuardRelease(&Guard);return Status;
}
#endif
