/* Exclusive native normal-pad lifetime. SPDX-License-Identifier: MIT */
#include <Library/GpioPinLeaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <oskal/cr_memory.h>
STATIC CR_STATUS Qualify(CONST CR_GPIO_PIN_LEASE_BANK *B,CONST GpioConfigParams *D,BOOLEAN RetainFields){
  if(!B||!B->Context||!D)return CR_INVALID_PARAMETER;
  CONST GpioDeviceContext *C=B->Context;
  if(!C->TlmmBaseAddress||!C->PinCount||C->PinCount>GPIO_PINS_MAX||D->PinNumber>=C->PinCount||
     D->OutputEnable>1||D->FunctionSel>GPIO_FUNC_MAX||D->Pull>GPIO_PULL_MAX||
     (D->DriveStrength>GPIO_DRIVE_STRENGTH_MAX&&!(RetainFields&&D->DriveStrength==GPIO_DRIVE_STRENGTH_UNCHANGE))||
     (D->OutputValue>GPIO_VALUE_MAX&&!(RetainFields&&D->OutputValue==GPIO_VALUE_UNCHANGE)))return CR_INVALID_PARAMETER;
  CONST GpioPinInfo *P=&C->GpioPins[D->PinNumber];CONST GpioConfigRegInfo *R=P->pRegCfg;
  if(P->Reserved)return CR_BUSY;
  if(!R||R->Index||!P->pTileInfo||!P->PinAddress||!R->MuxSelMsk||!R->PullMsk||!R->DriveStrengthMsk||
     !R->OutputEnableMsk||!R->InputMsk||!R->OutputMsk||!R->InterruptEnMsk||
     !(P->FuncMuxMap&(1U<<D->FunctionSel))||
     (C->EgpioFunction&&D->FunctionSel==C->EgpioFunction))return CR_UNSUPPORTED;
  UINT64 Offset=(UINT64)P->pTileInfo->TileOffset+(UINT64)D->PinNumber*GPIO_TLMM_PIN_STRIDE;
  if(C->TlmmBaseAddress>(UINT64)(~(UINTN)0)-Offset||P->PinAddress!=C->TlmmBaseAddress+Offset||
     P->PinAddress>(UINTN)~(UINTN)0-R->ControlRegOffset-4U||
     P->PinAddress>(UINTN)~(UINTN)0-R->IoRegOffset-4U||
     P->PinAddress>(UINTN)~(UINTN)0-R->IntCfgRegOffset-4U)return CR_INVALID_PARAMETER;
  return CR_SUCCESS;
}
STATIC CR_GPIO_PIN_LEASE *Find(CR_GPIO_PIN_LEASE_BANK *B,UINT16 Pin){
  if(!B||!B->Context)return NULL;
  for(UINT32 I=0;I<CR_GPIO_PIN_LEASE_CAPACITY;++I)
    if(B->Entries[I].Owner&&B->Entries[I].Baseline.PinNumber==Pin)return &B->Entries[I];
  return NULL;
}
STATIC BOOLEAN Same(CONST GpioConfigParams *A,CONST GpioConfigParams *B){
 return A->PinNumber==B->PinNumber&&A->OutputEnable==B->OutputEnable&&A->FunctionSel==B->FunctionSel&&
   A->Pull==B->Pull&&A->DriveStrength==B->DriveStrength&&A->OutputValue==B->OutputValue;
}
CR_STATUS CrGpioPinLeaseInit(CR_GPIO_PIN_LEASE_BANK *B,GpioDeviceContext *C){
  if(!B||!C||!C->TlmmBaseAddress||!C->PinCount||C->PinCount>GPIO_PINS_MAX)return CR_INVALID_PARAMETER;
  if(B->Context)return B->Context==C?CR_SUCCESS:CR_ALREADY_STARTED;
  for(UINT32 I=0;I<CR_GPIO_PIN_LEASE_CAPACITY;++I)if(B->Entries[I].Owner)return CR_ALREADY_STARTED;
  B->Context=C;return CR_SUCCESS;
}
BOOLEAN CrGpioPinLeaseOwned(CONST CR_GPIO_PIN_LEASE_BANK *B,UINT16 Pin){
  if(!B||!B->Context)return FALSE;
  for(UINT32 I=0;I<CR_GPIO_PIN_LEASE_CAPACITY;++I)
    if(B->Entries[I].Owner&&B->Entries[I].Baseline.PinNumber==Pin)return TRUE;
  return FALSE;
}
CR_STATUS CrGpioPinLeaseAcquire(CR_GPIO_PIN_LEASE_BANK *B,VOID *O,CONST GpioConfigParams *D,BOOLEAN *Owned){
  if(!Owned)return CR_INVALID_PARAMETER;*Owned=FALSE;
  if(!O)return CR_INVALID_PARAMETER;CR_STATUS S=Qualify(B,D,TRUE);if(S!=CR_SUCCESS)return S;
  CR_GPIO_PIN_LEASE *E=Find(B,D->PinNumber);
  GpioConfigParams Normalized=*D;
  if(E){
    if(E->Owner!=O)return CR_BUSY;*Owned=TRUE;
    if(Normalized.DriveStrength==GPIO_DRIVE_STRENGTH_UNCHANGE)Normalized.DriveStrength=E->Baseline.DriveStrength;
    if(Normalized.OutputValue==GPIO_VALUE_UNCHANGE)Normalized.OutputValue=E->Baseline.OutputValue;
    if(!Same(&Normalized,&E->Requested))return CR_INVALID_PARAMETER;
  }
  else {
    BOOLEAN Enabled=FALSE;S=GpioCheckInterruptEnabled(B->Context,D->PinNumber,&Enabled);
    if(S!=CR_SUCCESS)return S;if(Enabled)return CR_BUSY;
    for(UINT32 I=0;I<CR_GPIO_PIN_LEASE_CAPACITY;++I)if(!B->Entries[I].Owner){E=&B->Entries[I];break;}
    if(!E)return CR_OUT_OF_RESOURCES;
    GpioConfigParams Original={0};Original.PinNumber=D->PinNumber;GpioReadConfig(B->Context,&Original);
    Original.OutputValue=GpioReadOutputVal(B->Context,D->PinNumber);
    S=Qualify(B,&Original,FALSE);if(S!=CR_SUCCESS)return S;
    if(Normalized.DriveStrength==GPIO_DRIVE_STRENGTH_UNCHANGE)Normalized.DriveStrength=Original.DriveStrength;
    if(Normalized.OutputValue==GPIO_VALUE_UNCHANGE)Normalized.OutputValue=Original.OutputValue;
    S=Qualify(B,&Normalized,FALSE);if(S!=CR_SUCCESS)return S;
    E->Baseline=Original;E->Requested=Normalized;E->Owner=O;*Owned=TRUE;
  }
  E->Touched=TRUE;return GpioApplyConfigChecked(B->Context,&E->Requested);
}
CR_STATUS CrGpioPinLeaseSetValue(CR_GPIO_PIN_LEASE_BANK *B,VOID *O,UINT16 Pin,UINT8 V){
  if(!O||V>1)return CR_INVALID_PARAMETER;CR_GPIO_PIN_LEASE *E=Find(B,Pin);
  if(!E||E->Owner!=O)return CR_BUSY;if(!E->Requested.OutputEnable)return CR_UNSUPPORTED;
  E->Requested.OutputValue=(GpioValueType)V;E->Touched=TRUE;
  return GpioApplyConfigChecked(B->Context,&E->Requested);
}
CR_STATUS CrGpioPinLeaseReadValue(CR_GPIO_PIN_LEASE_BANK *B,VOID *O,UINT16 Pin,UINT8 *V){
  if(!O||!V)return CR_INVALID_PARAMETER;CR_GPIO_PIN_LEASE *E=Find(B,Pin);
  if(!E||E->Owner!=O)return CR_BUSY;CR_STATUS S=Qualify(B,&E->Requested,FALSE);if(S!=CR_SUCCESS)return S;
  *V=(UINT8)GpioReadInputVal(B->Context,Pin);return *V<=1?CR_SUCCESS:CR_DEVICE_ERROR;
}
STATIC CR_STATUS EdgeRegisters(CR_GPIO_PIN_LEASE_BANK *B,CR_GPIO_PIN_LEASE *E,UINTN *Cfg,UINTN *Status){
  CR_STATUS S=Qualify(B,&E->Requested,FALSE);if(S!=CR_SUCCESS)return S;
  CONST GpioPinInfo *P=&B->Context->GpioPins[E->Requested.PinNumber];
  CONST GpioConfigRegInfo *R=P->pRegCfg;
  /* Public pinctrl-msm common TLMM v2 layout; do not infer other layouts
     or interrupt-ack polarities. No board or pin identity is encoded here. */
  if(E->Requested.OutputEnable||E->Requested.FunctionSel!=GPIO_FUNC_NORMAL||
     R->IntCfgRegOffset!=8||R->IntTargetRegOffset!=8||R->IntStatusRegOffset!=12||
     R->InterruptEnMsk!=1||R->InterruptPolarityMsk!=2||R->InterruptDetectionMsk!=12||
     R->InterruptRawStatusMsk!=16||R->InterruptStatusMsk!=1||R->InterruptTargetMsk!=0xe0||
     R->TargetKpssVal!=3||R->InterruptDetectionWidth!=2)return CR_UNSUPPORTED;
  if(P->PinAddress>(UINTN)~(UINTN)0-16)return CR_INVALID_PARAMETER;
  *Cfg=P->PinAddress+8;*Status=P->PinAddress+12;return CR_SUCCESS;
}
CR_STATUS CrGpioPinLeaseArmFallingEdge(CR_GPIO_PIN_LEASE_BANK *B,VOID *O,UINT16 Pin){
  if(!O)return CR_INVALID_PARAMETER;CR_GPIO_PIN_LEASE *E=Find(B,Pin);
  if(!E||E->Owner!=O)return CR_BUSY;
  UINTN Cfg,Status;CR_STATUS S=EdgeRegisters(B,E,&Cfg,&Status);if(S!=CR_SUCCESS)return S;
  if(E->EdgeReady)return CrMmioRead32(Cfg)==E->PolledIrq?CR_SUCCESS:CR_NOT_READY;
  if(!E->IrqTouched){
    UINT32 V=CrMmioRead32(Cfg);
    /* An inherited raw latch may have a masked IRQ owner. Do not steal it. */
    if(V&0x11)return CR_BUSY;
    E->OriginalIrq=V;E->PolledIrq=(V&~0xffU)|0x7aU;E->IrqTouched=TRUE;
  }
  CrMmioWrite32(Cfg,E->PolledIrq);
  if(CrMmioRead32(Cfg)!=E->PolledIrq)return CR_DEVICE_ERROR;
  /* RAW_STATUS_EN can itself latch a spurious edge. Linux acknowledges
     after changing type; the consumer arms only after device setup. */
  CrMmioWrite32(Status,CrMmioRead32(Status)&~1U);
  (VOID)CrMmioRead32(Status);E->EdgeReady=TRUE;return CR_SUCCESS;
}
CR_STATUS CrGpioPinLeaseTakeEdge(CR_GPIO_PIN_LEASE_BANK *B,VOID *O,UINT16 Pin,BOOLEAN *Pending){
  if(!O||!Pending)return CR_INVALID_PARAMETER;*Pending=FALSE;
  CR_GPIO_PIN_LEASE *E=Find(B,Pin);if(!E||E->Owner!=O)return CR_BUSY;
  UINTN Cfg,Status;CR_STATUS S=EdgeRegisters(B,E,&Cfg,&Status);if(S!=CR_SUCCESS)return S;
  if(!E->EdgeReady||CrMmioRead32(Cfg)!=E->PolledIrq)return CR_NOT_READY;
  UINT32 V=CrMmioRead32(Status);if(!(V&1U))return CR_SUCCESS;
  CrMmioWrite32(Status,V&~1U);(VOID)CrMmioRead32(Status);
  *Pending=TRUE;return CR_SUCCESS;
}
CR_STATUS CrGpioPinLeaseRelease(CR_GPIO_PIN_LEASE_BANK *B,VOID *O,UINT16 Pin){
  if(!B||!B->Context||!O)return CR_INVALID_PARAMETER;CR_GPIO_PIN_LEASE *E=Find(B,Pin);
  if(!E)return CR_SUCCESS;if(E->Owner!=O)return CR_BUSY;
  CR_STATUS S=Qualify(B,&E->Baseline,FALSE);if(S!=CR_SUCCESS)return S;
  if(E->IrqTouched){
    UINTN Cfg,Status;S=EdgeRegisters(B,E,&Cfg,&Status);if(S!=CR_SUCCESS)return S;
    E->EdgeReady=FALSE;CrMmioWrite32(Cfg,E->OriginalIrq);
    if(CrMmioRead32(Cfg)!=E->OriginalIrq)return CR_DEVICE_ERROR;
    CrMmioWrite32(Status,CrMmioRead32(Status)&~1U);
    if(CrMmioRead32(Status)&1U)return CR_DEVICE_ERROR;
    E->IrqTouched=FALSE;
  }
  if(E->Touched){S=GpioApplyConfigChecked(B->Context,&E->Baseline);if(S!=CR_SUCCESS)return S;}
  ZeroMem(E,sizeof(*E));return CR_SUCCESS;
}
