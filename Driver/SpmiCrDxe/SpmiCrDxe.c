/* One native PMIC arbiter/PDC/IRQ owner; clients own only per-bit leases.
 * SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFICrSpmiProtocol.h>
#include <Protocol/EFICrFdtProtocol.h>
EFI_STATUS SpmiCrQualifyTypeC(CONST VOID *,UINTN,CONST SpmiDeviceContext *,CR_PMIC_CC_TARGET *);
STATIC CR_SPMI_SHARED mShared;
STATIC SpmiDeviceContext *mRaw;
STATIC PdcDeviceContext *mRoute;
STATIC CR_INTERRUPT_CONFIG mConfig;
STATIC CR_INTERRUPT_LEASE mLease;
STATIC EFI_CR_SPMI_PROTOCOL mProtocol;
STATIC CR_PMIC_CC_TARGET mTypeC;
STATIC EFI_STATUS mTypeCStatus=EFI_NOT_READY,mFault=EFI_NOT_READY;
STATIC EFI_EVENT mWork,mExit;
STATIC EFI_HANDLE mHandle;
STATIC UINT32 mPdcConfig,mPdcEnable,mSpiConfig;
STATIC UINT16 mSource;
STATIC BOOLEAN mPriorExternal,mPriorParentScan;
STATIC BOOLEAN mSaved,mRawOwned,mPublished,mRegistered,mSealed,mQueued,mRunning;
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC VOID Irq(VOID *X) {
 (VOID)X;if(!mRegistered)return;
 EFI_STATUS S=Exact(CrMaskInterrupt(&mLease));
 if(S!=EFI_SUCCESS){mFault=S;mSealed=TRUE;(VOID)CrSpmiSharedClose(&mShared);}
 if(mQueued)return;mQueued=TRUE;
 S=Exact(gBS->SignalEvent(mWork));if(S!=EFI_SUCCESS){mFault=S;mSealed=TRUE;(VOID)CrSpmiSharedClose(&mShared);}
}
STATIC VOID EFIAPI Work(EFI_EVENT E,VOID *X) {
 (VOID)X;CR_INTERRUPT_GUARD G;if(E!=mWork)return;
 EFI_STATUS S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS){
  /* The parent is already masked with an undrained snapshot. Revoke cached
   * client admission in RAM; retain IRQ, PMIC latches and queued ownership. */
  mFault=S;mSealed=TRUE;(VOID)CrSpmiSharedClose(&mShared);return;
 }
 if(!mQueued||mRunning){CrInterruptGuardRelease(&G);return;}
 mQueued=FALSE;mRunning=TRUE;
 if(!mSealed) {
  S=Exact(CrSpmiSharedDispatch(&mShared));
  if(S!=EFI_SUCCESS){mFault=S;mSealed=TRUE;(VOID)CrSpmiSharedClose(&mShared);}
 }
 if(!mSealed&&mRegistered){S=Exact(CrUnmaskInterrupt(&mLease));if(S!=EFI_SUCCESS){mFault=S;mSealed=TRUE;(VOID)CrSpmiSharedClose(&mShared);}}
 CrInterruptGuardRelease(&G);
 if(S!=EFI_SUCCESS)DEBUG((DEBUG_ERROR,"SpmiCr: IRQ source sealed (%r), ACK failures=%u orphan bits=%u\n",S,mRaw->IrqAckFailures,mRaw->IrqOrphans));
 mRunning=FALSE;
}
STATIC EFI_STATUS Add(CR_SPMI_REGION *R,UINTN *Count,UINT8 Sid,UINT8 Peripheral,UINT8 Access,UINT8 Irqs) {
 for(UINTN I=0;I<*Count;I++)if(R[I].Sid==Sid&&R[I].Peripheral==Peripheral){R[I].Access|=Access;R[I].Irqs|=Irqs;return EFI_SUCCESS;}
 if(*Count==CR_SPMI_MAX_REGIONS||Sid>15)return EFI_UNSUPPORTED;
 R[(*Count)++]=(CR_SPMI_REGION){Sid,Peripheral,Access,Irqs};return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Acquire(EFI_CR_SPMI_PROTOCOL *This,EFI_CR_SPMI_ROLE Role,VOID *Owner,CR_SPMI_CLIENT *T,CR_SPMI_BUS *Bus) {
 if(This!=&mProtocol)return EFI_INVALID_PARAMETER;
 CR_SPMI_REGION R[CR_SPMI_MAX_REGIONS];UINTN N=0;EFI_STATUS S;
 if(mSealed||!mPublished)return EFI_NOT_READY;
 if(Role==CrSpmiClientButtons) {
  CONST ButtonsTargetContext *B=CrDalGetButtonsContext();CONST PmicGpioTargetContext *P=CrDalGetPmicGpioContext();
  if(!B||!P||!B->Buttons||B->ButtonCount>BUTTON_CODE_MAX)return EFI_NOT_FOUND;
  /* PmicGpioInitialize reads each declared pin; only actual button pins receive
     write/IRQ rights. Unused GPIO pins remain read-only. */
  for(UINTN I=0;I<P->ControllerCount;I++)for(UINTN J=0;J<P->Controllers[I].PinCount;J++) {
   S=Add(R,&N,P->Controllers[I].Sid,(UINT8)((P->Controllers[I].PeripheralBase>>8)+J),CR_SPMI_READ,0);if(S!=EFI_SUCCESS)return S;
  }
  for(UINTN I=0;I<B->ButtonCount;I++) {
   CONST ButtonTarget *V=&B->Buttons[I];if(V->Irq>7)return EFI_UNSUPPORTED;
   S=Add(R,&N,V->Sid,V->Peripheral,CR_SPMI_READ|(V->Source==BUTTON_SOURCE_PMIC_GPIO?CR_SPMI_WRITE:0),(UINT8)BIT(V->Irq));if(S!=EFI_SUCCESS)return S;
  }
 }else if(Role==CrSpmiClientTypeC) {
  if(mTypeCStatus!=EFI_SUCCESS)return mTypeCStatus;
  R[N++]=(CR_SPMI_REGION){2,1,CR_SPMI_READ,0};R[N++]=(CR_SPMI_REGION){2,0x15,CR_SPMI_READ,0xff};R[N++]=(CR_SPMI_REGION){2,0x17,CR_SPMI_READ,0};
 }else return EFI_UNSUPPORTED;
 return CrSpmiSharedAcquire(&mShared,Owner,R,N,T,Bus);
}
STATIC EFI_STATUS EFIAPI Release(EFI_CR_SPMI_PROTOCOL *This,CR_SPMI_CLIENT *T){return This==&mProtocol?CrSpmiSharedRelease(&mShared,T):EFI_INVALID_PARAMETER;}
STATIC EFI_STATUS EFIAPI GetTypeC(EFI_CR_SPMI_PROTOCOL *This,CR_PMIC_CC_TARGET *T){
 if(This!=&mProtocol||!T)return EFI_INVALID_PARAMETER;if(mSealed)return EFI_NOT_READY;
 if(mTypeCStatus==EFI_SUCCESS)*T=mTypeC;return mTypeCStatus;
}
STATIC EFI_STATUS WordFits(UINTN Base,UINTN Size,UINTN Offset) {
 if(!Base||(Base&3)||(Size&3)||Size<4||Offset>Size-4||Base>(UINTN)-1-(Size-1))return EFI_INVALID_PARAMETER;
 return EFI_SUCCESS;
}
STATIC EFI_STATUS RestoreRoute(VOID) {
 UINT32 Pin=mRaw->PdcPinNumber,Mask=1U<<(Pin&31);
 UINTN Config=mRoute->BaseAddress+0x110+Pin*4,Enable=mRoute->BaseAddress+0x10+(Pin>>5)*4;
 MmioAndThenOr32(Config,~7U,mPdcConfig&7);
 if((MmioRead32(Config)&7)!=(mPdcConfig&7))return EFI_DEVICE_ERROR;
 if(mRoute->SPIConfigBase){UINT32 Bit=1U<<((mSource-32)&31);UINTN Spi=mRoute->SPIConfigBase+((mSource-32)>>5)*4;
  MmioAndThenOr32(Spi,~Bit,mSpiConfig);
  if((MmioRead32(Spi)&Bit)!=mSpiConfig)return EFI_DEVICE_ERROR;
 }
 MmioAndThenOr32(Enable,~Mask,mPdcEnable);
 if((MmioRead32(Enable)&Mask)!=mPdcEnable)return EFI_DEVICE_ERROR;
 mSaved=FALSE;return EFI_SUCCESS;
}
STATIC EFI_STATUS Stop(VOID) {
 CR_INTERRUPT_GUARD G;EFI_STATUS S;
 /* No IRQ provider is needed to retire events from a pre-hardware failure. */
 if(!mRegistered&&!mSaved&&!mRawOwned&&!mShared.Initialized)goto Events;
 S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS)return S;
 mSealed=TRUE;if(mShared.Initialized)(VOID)CrSpmiSharedClose(&mShared);
 if(mRegistered){S=Exact(CrUnregisterInterrupt(&mConfig));mRegistered=CrInterruptIsRegistered(&mConfig);
  if(S!=EFI_SUCCESS||mRegistered){CrInterruptGuardRelease(&G);return S==EFI_SUCCESS?EFI_NOT_READY:S;}}
 if(mQueued||mRunning){CrInterruptGuardRelease(&G);return EFI_NOT_READY;}
 if(mShared.Initialized){S=CrSpmiSharedUninit(&mShared);if(S!=EFI_SUCCESS){CrInterruptGuardRelease(&G);return S;}}
 if(mRawOwned){S=Exact(SpmiLibDeinit(mRaw));if(S!=EFI_SUCCESS){CrInterruptGuardRelease(&G);return S;}mRaw->InterruptExternallyManaged=mPriorExternal;
  mRaw->SharedParentIrqScan=mPriorParentScan;mRawOwned=FALSE;}
 if(mSaved){S=RestoreRoute();if(S!=EFI_SUCCESS){CrInterruptGuardRelease(&G);return S;}}
 CrInterruptGuardRelease(&G);
Events:
 if(mWork){S=Exact(gBS->CloseEvent(mWork));if(S!=EFI_SUCCESS)return S;mWork=NULL;}
 if(mExit){S=Exact(gBS->CloseEvent(mExit));if(S!=EFI_SUCCESS)return S;mExit=NULL;}
 return EFI_SUCCESS;
}
STATIC VOID EFIAPI Exit(EFI_EVENT E,VOID *X){(VOID)E;(VOID)X;(VOID)Stop();}
EFI_STATUS EFIAPI SpmiCrUnload(EFI_HANDLE Image){
 (VOID)Image;if(mRunning)return EFI_NOT_READY;EFI_STATUS S=Stop();if(S!=EFI_SUCCESS)return S;
 if(mPublished){S=Exact(gBS->UninstallMultipleProtocolInterfaces(mHandle,&gEfiCrSpmiProtocolGuid,&mProtocol,NULL));if(S==EFI_SUCCESS)mPublished=FALSE;}return S;
}
EFI_STATUS EFIAPI SpmiCrEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System){
 (VOID)Image;(VOID)System;CR_INTERRUPT_GUARD G;EFI_STATUS S,Cleanup;CR_INTERRUPT_TRIGGER_TYPE Trigger;
 if(mPublished||mRawOwned||mSaved||mWork||mExit)return EFI_ALREADY_STARTED;
 mRaw=CrDalGetSpmiContext();mRoute=CrDalGetPdcContext();if(!mRaw||!mRoute)return EFI_NOT_FOUND;
 if(mRaw->Initialized||mRaw->InterruptRegistered)return EFI_ALREADY_STARTED;
 mSealed=FALSE;mFault=EFI_SUCCESS;
 EFI_CR_FDT_PROTOCOL *F=NULL;CONST VOID *Blob=NULL;UINTN Bytes=0;
 mTypeCStatus=gBS->LocateProtocol(&gEfiCrFdtProtocolGuid,NULL,(VOID **)&F);
 if(mTypeCStatus==EFI_SUCCESS && F && F->Revision==EFI_CR_FDT_PROTOCOL_REVISION && F->Size>=sizeof(*F)) {
  mTypeCStatus=F->GetBlob(F,&Blob,&Bytes);if(mTypeCStatus==EFI_SUCCESS)mTypeCStatus=SpmiCrQualifyTypeC(Blob,Bytes,mRaw,&mTypeC);
 }else if(mTypeCStatus==EFI_SUCCESS)mTypeCStatus=EFI_UNSUPPORTED;
 S=Exact(gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Work,NULL,&mWork));if(S!=EFI_SUCCESS)goto Failure;
 S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,&gEfiEventExitBootServicesGuid,&mExit));if(S!=EFI_SUCCESS)goto Failure;
 S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS)goto Failure;
 if(mRaw->PdcPinNumber>MAX_UINT16){S=EFI_UNSUPPORTED;goto GuardFailure;}
 S=PdcGetGicIrqFromPdcPin(mRoute,(UINT16)mRaw->PdcPinNumber,&mSource);
 if(S!=EFI_SUCCESS||mSource<32){if(S==EFI_SUCCESS)S=EFI_UNSUPPORTED;goto GuardFailure;}
 UINT32 Pin=mRaw->PdcPinNumber;
 S=WordFits(mRoute->BaseAddress,mRoute->Size,0x110+(UINTN)Pin*4);
 if(S==EFI_SUCCESS)S=WordFits(mRoute->BaseAddress,mRoute->Size,0x10+(Pin>>5)*4);
 if(S==EFI_SUCCESS&&mRoute->SPIConfigBase)S=WordFits(mRoute->SPIConfigBase,mRoute->SPIConfigSize,((mSource-32)>>5)*4);
 if(S!=EFI_SUCCESS)goto GuardFailure;
 mPdcConfig=MmioRead32(mRoute->BaseAddress+0x110+Pin*4);
 mPdcEnable=MmioRead32(mRoute->BaseAddress+0x10+(Pin>>5)*4)&(1U<<(Pin&31));mSpiConfig=0;
 if(mRoute->SPIConfigBase)mSpiConfig=MmioRead32(mRoute->SPIConfigBase+((mSource-32)>>5)*4)&(1U<<((mSource-32)&31));
 mSaved=TRUE;S=PdcEnableInterrupt(mRoute,(UINT16)Pin,FALSE);
 if(S==EFI_SUCCESS && (MmioRead32(mRoute->BaseAddress+0x10+(Pin>>5)*4)&(1U<<(Pin&31))))S=EFI_DEVICE_ERROR;
 if(S==EFI_SUCCESS)S=PdcGicSetType(mRoute,(UINT16)Pin,CR_INTERRUPT_TRIGGER_LEVEL_HIGH,&Trigger);
 /* PdcLib reports submitted MMIO writes; this owner requires readback before
    enabling its upstream source or exposing any client interface. */
 if(S==EFI_SUCCESS && (MmioRead32(mRoute->BaseAddress+0x110+Pin*4)&7)!=4)S=EFI_DEVICE_ERROR;
 if(S==EFI_SUCCESS && mRoute->SPIConfigBase &&
    !(MmioRead32(mRoute->SPIConfigBase+((mSource-32)>>5)*4)&(1U<<((mSource-32)&31))))S=EFI_DEVICE_ERROR;
 if(S!=EFI_SUCCESS)goto GuardFailure;
 mPriorExternal=mRaw->InterruptExternallyManaged;mPriorParentScan=mRaw->SharedParentIrqScan;
 mRaw->InterruptExternallyManaged=TRUE;mRaw->SharedParentIrqScan=TRUE;mRawOwned=TRUE;S=SpmiLibInit(&mRaw);if(S!=EFI_SUCCESS)goto GuardFailure;
 S=CrSpmiSharedInit(&mShared,mRaw);if(S!=EFI_SUCCESS)goto GuardFailure;
 mConfig.InterruptNumber=mSource;mConfig.TriggerType=Trigger;mConfig.Handler=Irq;mConfig.Param=&mShared;
 S=Exact(CrRegisterInterruptLeased(&mConfig,&mLease));mRegistered=CrInterruptIsRegistered(&mConfig);
 if(S==EFI_SUCCESS&&!mRegistered)S=EFI_NOT_READY;
 if(S==EFI_SUCCESS)S=PdcEnableInterrupt(mRoute,(UINT16)Pin,TRUE);
 if(S==EFI_SUCCESS && !(MmioRead32(mRoute->BaseAddress+0x10+(Pin>>5)*4)&(1U<<(Pin&31))))S=EFI_DEVICE_ERROR;
GuardFailure:
 CrInterruptGuardRelease(&G);if(S!=EFI_SUCCESS)goto Failure;
 mProtocol=(EFI_CR_SPMI_PROTOCOL){EFI_CR_SPMI_PROTOCOL_REVISION,sizeof(mProtocol),Acquire,Release,GetTypeC};
 S=Exact(gBS->InstallMultipleProtocolInterfaces(&mHandle,&gEfiCrSpmiProtocolGuid,&mProtocol,NULL));if(S!=EFI_SUCCESS)goto Failure;
 mPublished=TRUE;DEBUG((DEBUG_WARN,"SpmiCr: shared IRQ source=%u; direct PM8150B observer=%r\n",mSource,mTypeCStatus));return EFI_SUCCESS;
Failure:
 Cleanup=Stop();DEBUG((DEBUG_ERROR,"SpmiCr: start=%r cleanup=%r\n",S,Cleanup));return Cleanup==EFI_SUCCESS?S:EFI_SUCCESS;
}
