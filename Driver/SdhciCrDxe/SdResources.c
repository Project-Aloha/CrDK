/* Explicit native SDCC2 target resources; native protocols adapt the shared owner cores.
 * SPDX-License-Identifier: MIT */
#include "SdResources.h"
#include <Library/DebugLib.h>
#include <Library/CrDalLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>

typedef struct { CONST CHAR8 *Name; BOOLEAN Owned; EFI_STATUS Status; } SD_VOTE;
STATIC EFI_CLOCK_CR_PROTOCOL *mClock;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mIcb;
STATIC EFI_RPMH_CR_PROTOCOL *mRail;
STATIC EFI_GPIO_CR_PROTOCOL *mGpio;
STATIC SD_VOTE mClocks[2];
STATIC SD_VOTE mRails[2];
STATIC INTERCONNECT_PATH_HANDLE mPaths[2];
STATIC CONST CR_TARGET_SD_CONTEXT *mSdTarget;
STATIC CR_SDHC *mController;
STATIC EFI_EVENT mPowerWork;
STATIC BOOLEAN mGpioOwned,mAccess,mBusy,mClosing,mFrozen,mIrqOwned;
STATIC BOOLEAN mHostMayLive,mHostStopped;
STATIC BOOLEAN mIrqPaused;
STATIC UINT32 mRate;
STATIC EFI_STATUS mPowerStatus=EFI_SUCCESS;
STATIC BOOLEAN mRequestActive,mBusValid,mBusOn,mIoValid,mIoHigh,mInternalSync;
STATIC UINT64 mIoSequence,mSyncBefore;
STATIC UINT32 mSyncRequest;
STATIC BOOLEAN mSyncPrior;
STATIC EFI_STATUS mIoStatus=EFI_NOT_READY;
STATIC EFI_STATUS SyncPower(CR_SDHC *C,UINT32 Request,BOOLEAN Begin,EFI_STATUS WriteStatus);
STATIC UINT64 mBusSequence;
STATIC EFI_STATUS mBusStatus=EFI_NOT_READY;
STATIC VOID PowerIrq(VOID *Context);
STATIC EFI_STATUS DrainIrq(CR_SDHC *C,BOOLEAN Closing);
STATIC CR_INTERRUPT_CONFIG mIrq={.Param=NULL,.Handler=PowerIrq,
  .InterruptNumber=0,.TriggerType=CR_INTERRUPT_TRIGGER_LEVEL_HIGH};

/* Ownership admissions alone are protected at NOTIFY. Fallible protocol
 * votes execute at the caller's original APPLICATION/CALLBACK priority. */
STATIC EFI_STATUS Enter(BOOLEAN Cleanup) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY); EFI_STATUS S=EFI_SUCCESS;
  if(T>TPL_CALLBACK||mFrozen) S=EFI_ACCESS_DENIED;
  else if(mBusy) S=EFI_NOT_READY;
  else if(mClosing&&!Cleanup) S=EFI_NOT_READY;
  else mBusy=TRUE;
  gBS->RestoreTPL(T); return S;
}
STATIC EFI_STATUS Leave(EFI_STATUS S) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY); mBusy=FALSE; gBS->RestoreTPL(T); return S;
}
EFI_STATUS CrSdResourcesReady(VOID) {
  /* Freeze deliberately retains access for the adapter's final synchronous
   * controller stop; its public methods have their own Exiting admission. */
  return mAccess&&mController&&mSdTarget&&mClocks[0].Owned&&mClocks[1].Owned&&
    mClocks[0].Status==EFI_SUCCESS&&mClocks[1].Status==EFI_SUCCESS?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS ClockOff(UINT32 I) {
  SD_VOTE *V=&mClocks[I]; if(!V->Owned)return EFI_SUCCESS;
  V->Status=mClock->SetClock(mClock,mSdTarget->ClockController,V->Name,0,FALSE);
  if(V->Status==EFI_SUCCESS)V->Owned=FALSE;
  return V->Status;
}
STATIC EFI_STATUS ClockOn(UINT32 I,UINT32 Rate) {
  SD_VOTE *V=&mClocks[I];
  V->Status=CrAcquireClock(mClock,mSdTarget->ClockController,V->Name,Rate,&V->Owned);
  if(V->Owned)mHostMayLive=TRUE;
  return V->Status;
}
STATIC EFI_STATUS RailOff(UINT32 I) {
  SD_VOTE *V=&mRails[I];if(!V->Owned)return EFI_SUCCESS;
  V->Status=mRail->RpmhEnableVreg(mRail,V->Name,FALSE);
  if(V->Status==EFI_SUCCESS)V->Owned=FALSE;
  return V->Status;
}
STATIC EFI_STATUS RailOn(UINT32 I) {
  SD_VOTE *V=&mRails[I];
  if(V->Owned&&V->Status==EFI_SUCCESS)return EFI_SUCCESS;
  if(V->Owned){EFI_STATUS S=RailOff(I);if(S!=EFI_SUCCESS)return S;}
  V->Status=mRail->AcquireVreg(mRail,V->Name,mSdTarget->Rails[I].VoltageMv,mSdTarget->Rails[I].Mode,&V->Owned);
  if(V->Status!=EFI_SUCCESS)
    DEBUG((DEBUG_ERROR,"SdhciCr: SD supply %a %umV/HPM: %r owned=%u\n",V->Name,mSdTarget->Rails[I].VoltageMv,V->Status,V->Owned));
  return V->Status;
}
STATIC VOID EFIAPI PowerWork(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(mFrozen||mClosing||!mController)return;
  EFI_STATUS S=CrSdResourcesServicePower(mController);
  if(S!=EFI_SUCCESS&&S!=EFI_NOT_READY)
    DEBUG((DEBUG_ERROR,"SdhciCr: power service: %r pending=%x\n",S,mController->PowerPending));
}
STATIC VOID PowerIrq(VOID *Context) {
  (VOID)Context;
  /* OSKAL invokes this at HIGH through EOI. Only bounded MMIO capture and
   * event enqueue occur here; no regulator/clock/pad RPC can run in IRQ. */
  if(mFrozen||mClosing||!mController||!mAccess)return;
  UINT32 Observed=0;
  mPowerStatus=CrSdhcPowerCapture(mController,&Observed);
  if(Observed||mPowerStatus!=EFI_SUCCESS)(VOID)gBS->SignalEvent(mPowerWork);
}
EFI_STATUS CrSdResourcesAcquire(CR_SDHC *C) {
  if(!C||!C->Initialized)return EFI_INVALID_PARAMETER;
  CONST CR_TARGET_SD_CONTEXT *Target=CrDalGetSdContext();
  if(!Target)return EFI_UNSUPPORTED;
  if(!CrTargetSdValid(Target)||C->Io.MappingSize!=Target->Bytes)return EFI_COMPROMISED_DATA;
  EFI_STATUS S=Enter(FALSE);if(S!=EFI_SUCCESS)return S;
  if(mController)return Leave(EFI_ALREADY_STARTED);
  /* Commit the resident owner before the first fallible side effect. */
  mSdTarget=Target;
  for(UINT32 I=0;I<2;++I){mClocks[I].Name=Target->Clocks[I];mRails[I].Name=Target->Rails[I].Name;}
  mIrq.InterruptNumber=Target->PowerInterrupt;mIrq.TriggerType=Target->PowerTrigger;
  mController=C;C->SyncPower=SyncPower;
  S=gBS->LocateProtocol(&gEfiClockCrProtocolGuid,NULL,(VOID **)&mClock);if(S!=EFI_SUCCESS)goto Out;
  S=gBS->LocateProtocol(&gEfiInterconnectCrProtocolGuid,NULL,(VOID **)&mIcb);if(S!=EFI_SUCCESS)goto Out;
  S=gBS->LocateProtocol(&gEfiRpmhCrProtocolGuid,NULL,(VOID **)&mRail);if(S!=EFI_SUCCESS)goto Out;
  S=gBS->LocateProtocol(&gEfiGpioCrProtocolGuid,NULL,(VOID **)&mGpio);if(S!=EFI_SUCCESS)goto Out;
  if(!mClock||!mClock->SetClock||!mIcb||!mIcb->AcquirePath||!mIcb->SetBandwidth||!mIcb->ReleasePath||
     !mRail||mRail->Revision<EFI_RPMH_CR_PROTOCOL_REVISION||!mRail->EnsureBootFloor||!mRail->AcquireVreg||!mRail->RpmhEnableVreg||!mGpio||mGpio->Revision<EFI_GPIO_CR_PROTOCOL_REVISION||
     !mGpio->AcquireSd||!mGpio->SetSdActive||!mGpio->ReadSdCardDetect||!mGpio->ReleaseSd){S=EFI_UNSUPPORTED;goto Out;}
  /* Presence/revision alone is insufficient: the RPMh driver can publish
   * after a boot-floor warning. Require its exact current six-resource ACK
   * before clocks, including CX and XO3, without adding a seal-blocking owner. */
  S=mRail->EnsureBootFloor(mRail);
  if(S!=EFI_SUCCESS){if(!EFI_ERROR(S))S=EFI_NOT_READY;goto Out;}
  for(UINT32 I=0;I<2;++I){
    S=mIcb->AcquirePath(mIcb,Target->Paths[I].Provider,Target->Paths[I].Source,Target->Paths[I].Destination,&mPaths[I]);if(S!=EFI_SUCCESS)goto Out;
    /* DT100MHz ceiling OPP in sdhc-ddr/cpu-sdhc order, Linux kB/s units. */
    S=mIcb->SetBandwidth(mIcb,mPaths[I],Target->Paths[I].Average,Target->Paths[I].Peak);if(S!=EFI_SUCCESS)goto Out;
  }
  S=ClockOn(0,0);if(S!=EFI_SUCCESS)goto Out;
  S=ClockOn(1,400000);if(S!=EFI_SUCCESS)goto Out;mRate=400000;mAccess=TRUE;
  S=mGpio->AcquireSd(mGpio,C,&mGpioOwned);if(S!=EFI_SUCCESS)goto Out;
  if(!mGpioOwned){S=EFI_NOT_READY;goto Out;}
  mAccess=TRUE;
  S=CrSdhcPowerMask(C,FALSE);if(S!=EFI_SUCCESS)goto Out;
  S=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,PowerWork,NULL,&mPowerWork);if(S!=EFI_SUCCESS)goto Out;
  S=CrRegisterInterrupt(&mIrq);mIrqOwned=CrInterruptIsRegistered(&mIrq);
  if(S==EFI_SUCCESS&&!mIrqOwned)S=EFI_NOT_READY;
  /* Adapter resets inherited host/DMA before enabling the power causes. */
Out:
  DEBUG((S==EFI_SUCCESS?DEBUG_INFO:DEBUG_ERROR,"SdhciCr: resources IRQ%u clocks=%u/%u pads=%u: %r\n",
    mIrq.InterruptNumber,mClocks[0].Owned,mClocks[1].Owned,mGpioOwned,S));
  return Leave(S);
}
EFI_STATUS CrSdResourcesSetClock(UINT32 Hz) {
  if(Hz!=400000&&Hz!=25000000&&Hz!=50000000)return EFI_UNSUPPORTED;
  EFI_STATUS S=Enter(FALSE);if(S!=EFI_SUCCESS)return S;
  if(!mController||!mClocks[0].Owned||!mGpioOwned)return Leave(EFI_NOT_READY);
  if(mRate==Hz&&mClocks[1].Owned&&mClocks[1].Status==EFI_SUCCESS&&!mIrqPaused)return Leave(EFI_SUCCESS);
  /* Mask/remove the actual level IRQ while mappings become inaccessible.
   * Pending power causes remain latched and are observed after reacquire. */
  mIrqPaused=TRUE;S=DrainIrq(mController,FALSE);if(S!=EFI_SUCCESS)return Leave(S);
  /* Caller disabled card clock. A native SetClock(TRUE) would obtain another
   * reference, so retire this exact owned apps reference before reacquiring. */
  mAccess=FALSE;S=ClockOff(1);if(S!=EFI_SUCCESS)return Leave(S);
  if(mFrozen)return Leave(EFI_ACCESS_DENIED);
  S=ClockOn(1,Hz);
  if(S==EFI_SUCCESS){
    mRate=Hz;mAccess=TRUE;
    if(mFrozen)return Leave(EFI_ACCESS_DENIED);
    S=CrRegisterInterrupt(&mIrq);mIrqOwned=CrInterruptIsRegistered(&mIrq);
    if(S==EFI_SUCCESS&&!mIrqOwned)S=EFI_NOT_READY;
    if(S==EFI_SUCCESS)S=CrSdhcPowerMask(mController,TRUE);
    if(S==EFI_SUCCESS)mIrqPaused=FALSE;
  }
  return Leave(S);
}
EFI_STATUS CrSdResourcesCardPresent(BOOLEAN *Present) {
  if(!Present)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(FALSE);if(S!=EFI_SUCCESS)return S;
  if(!mController||!mGpioOwned||CrSdResourcesReady()!=EFI_SUCCESS)return Leave(EFI_NOT_READY);
  /* GPIO returns raw electrical HIGH; the explicit board profile supplies polarity. */
  BOOLEAN High=FALSE;S=mGpio->ReadSdCardDetect(mGpio,mController,&High);
  if(S==EFI_SUCCESS)*Present=High!=mSdTarget->CardDetectActiveLow;
  return Leave(S);
}
STATIC EFI_STATUS ServicePowerLocked(CR_SDHC *C) {
  EFI_STATUS S;
  if(CrSdResourcesReady()!=EFI_SUCCESS)return EFI_NOT_READY;
  CR_INTERRUPT_GUARD G={0}; S=CrInterruptGuardAcquire(&G);
  if(S!=EFI_SUCCESS)return S;
  UINT32 Observed=0;S=CrSdhcPowerCapture(C,&Observed);mPowerStatus=S;
  UINT32 Causes=C->PowerPending;CrInterruptGuardRelease(&G);
  if(S!=EFI_SUCCESS)return S;
  if(!Causes)return EFI_SUCCESS;
  if((Causes&3)==3||(Causes&12)==12)S=EFI_COMPROMISED_DATA;
  else if((Causes&CR_SDHC_IO_LOW)&&!(Causes&CR_SDHC_POWER_OFF))S=EFI_UNSUPPORTED;
  else {
    if(Causes&3){BOOLEAN On=(Causes&CR_SDHC_POWER_ON)!=0;
      /* Source order supplies before active pads, sleep pads before OFF. */
      if(!On)S=mGpio->SetSdActive(mGpio,C,FALSE);
      for(UINT32 I=0;S==EFI_SUCCESS&&I<2;++I){
        if(mFrozen){S=EFI_ACCESS_DENIED;break;}
        S=On?RailOn(I):RailOff(1-I);
      }
      if(S==EFI_SUCCESS&&On&&!mFrozen)S=mGpio->SetSdActive(mGpio,C,TRUE);
    }
    if(S==EFI_SUCCESS&&(Causes&(CR_SDHC_POWER_ON|CR_SDHC_IO_HIGH))&&!mFrozen)
      S=CrSdhcIoPadHigh(C,TRUE);
  }
  if(mFrozen)return EFI_ACCESS_DENIED;
  /* Completion synchronizes against ISR between unmask and pending clear.
   * Failed effects receive FAIL ACK, never success. Exact pending is retained
   * if capture/ACK/unmask failed and can be retried with the original frame. */
  EFI_STATUS Effect=S,Finish=CrInterruptGuardAcquire(&G);
  if(Finish==EFI_SUCCESS){Finish=CrSdhcPowerComplete(C,Causes,S==EFI_SUCCESS,S==EFI_SUCCESS);CrInterruptGuardRelease(&G);}
  mPowerStatus=Finish==EFI_SUCCESS?Effect:Finish;
  if(Causes&3){
    mBusValid=mPowerStatus==EFI_SUCCESS;
    mBusOn=(Causes&CR_SDHC_POWER_ON)!=0;
    mBusStatus=mPowerStatus;++mBusSequence;
  }
  if(Causes&15){
    mIoValid=mPowerStatus==EFI_SUCCESS;
    mIoHigh=(Causes&(CR_SDHC_POWER_ON|CR_SDHC_IO_HIGH))!=0;
    mIoStatus=mPowerStatus;++mIoSequence;
  }
  return mPowerStatus;
}
EFI_STATUS CrSdResourcesServicePower(CR_SDHC *C) {
  if(!C||C!=mController)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(FALSE);if(S!=EFI_SUCCESS)return S;
  return Leave(ServicePowerLocked(C));
}
/* Begin/End bracket a single host register write. The caller may already
 * be at CALLBACK, so the queued power worker cannot be its progress source.
 * StopHost uses the same locked engine after draining IRQ/deferred admission. */
STATIC EFI_STATUS SyncPower(CR_SDHC *C,UINT32 Request,BOOLEAN Begin,EFI_STATUS WriteStatus) {
  if(!C||C!=mController||(Request!=CR_SDHC_POWER_OFF&&Request!=CR_SDHC_IO_HIGH))return EFI_INVALID_PARAMETER;
  if(mFrozen)return EFI_ACCESS_DENIED;
  EFI_STATUS S=EFI_SUCCESS;
  BOOLEAN Internal=mInternalSync;
  if(!Internal){S=Enter(FALSE);if(S!=EFI_SUCCESS)return S;}
  else if(!mBusy)return EFI_ACCESS_DENIED;
  if(Begin){
    if(mRequestActive){S=EFI_NOT_READY;goto Out;}
    S=ServicePowerLocked(C);if(S!=EFI_SUCCESS)goto Out;
    UINT32 Previous=0;
    S=CrSdhcRead(C,Request==CR_SDHC_POWER_OFF?0x29:0x3e,Request==CR_SDHC_POWER_OFF?1:2,&Previous);
    if(S!=EFI_SUCCESS)goto Out;
    mSyncBefore=Request==CR_SDHC_POWER_OFF?mBusSequence:mIoSequence;
    mSyncPrior=Request==CR_SDHC_POWER_OFF?(mBusValid&&!mBusOn):(mIoValid&&mIoHigh);
    mSyncPrior=mSyncPrior&&!(Previous&(Request==CR_SDHC_POWER_OFF?BIT0:BIT3));
    mRequestActive=TRUE;mSyncRequest=Request;
  }else{
    if(!mRequestActive||mSyncRequest!=Request){S=EFI_INVALID_PARAMETER;goto Out;}
    S=WriteStatus;
    if(S==EFI_SUCCESS){
      S=EFI_TIMEOUT;
      for(UINT32 Us=0;Us<5000000U;Us+=10){
        S=ServicePowerLocked(C);if(S!=EFI_SUCCESS)break;
        BOOLEAN Match=Request==CR_SDHC_POWER_OFF?(mBusValid&&!mBusOn):(mIoValid&&mIoHigh);
        UINT64 Sequence=Request==CR_SDHC_POWER_OFF?mBusSequence:mIoSequence;
        EFI_STATUS Effect=Request==CR_SDHC_POWER_OFF?mBusStatus:mIoStatus;
        if(Match&&(mSyncPrior||Sequence!=mSyncBefore)){
          /* BUS_OFF ACK releases RESET_ALL; its reset/power readback comes
           * after the core's reset-bit poll, not before that poll can run. */
          if(Request==CR_SDHC_IO_HIGH){UINT32 V=0;S=CrSdhcRead(C,0x3e,2,&V);
            if(S==EFI_SUCCESS&&(V&BIT3))S=EFI_DEVICE_ERROR;}
          if(S==EFI_SUCCESS)S=Effect;break;
        }
        MicroSecondDelay(10);S=EFI_TIMEOUT;
      }
    }
    mRequestActive=FALSE;mSyncRequest=0;
  }
Out:
  return Internal?S:Leave(S);
}
STATIC EFI_STATUS RequestDone(EFI_STATUS S) {
  EFI_TPL T=gBS->RaiseTPL(TPL_NOTIFY);mRequestActive=FALSE;gBS->RestoreTPL(T);return S;
}
EFI_STATUS CrSdResourcesRequestPower(CR_SDHC *C,BOOLEAN On) {
  if(!C||C!=mController||On>1)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(FALSE);if(S!=EFI_SUCCESS)return S;
  if(mRequestActive)return Leave(EFI_NOT_READY);
  mRequestActive=TRUE;Leave(EFI_SUCCESS);
  /* Settle any stale IRQ frame before selecting this request's generation. */
  S=CrSdResourcesServicePower(C);if(S!=EFI_SUCCESS)return RequestDone(S);
  S=Enter(FALSE);if(S!=EFI_SUCCESS)return RequestDone(S);
  UINT32 Power=0;S=CrSdhcRead(C,0x29,1,&Power);
  if(S!=EFI_SUCCESS){Leave(S);return RequestDone(S);}
  if(mBusValid&&mBusOn==On&&Power==(On?0x0dU:0U)){
    Leave(EFI_SUCCESS);return RequestDone(EFI_SUCCESS);
  }
  UINT64 Before=mBusSequence;
  S=CrSdhcPowerRequest(C,On);Leave(S);
  if(S!=EFI_SUCCESS)return RequestDone(S);
  /* No lock/TPL_NOTIFY spans this wait. Inline service guarantees progress
   * when an MmcDxe caller is already at the queued worker's CALLBACK TPL. */
  for(UINT32 Us=0;Us<5000000U;Us+=10){
    S=CrSdResourcesServicePower(C);
    if(S!=EFI_SUCCESS&&S!=EFI_NOT_READY)return RequestDone(S);
    if(mFrozen)return RequestDone(EFI_ACCESS_DENIED);
    if(mBusSequence!=Before&&mBusOn==On){
      if(mBusStatus!=EFI_SUCCESS)return RequestDone(mBusStatus);
      Power=0;S=CrSdhcRead(C,0x29,1,&Power);
      return RequestDone(S!=EFI_SUCCESS?S:Power==(On?0x0dU:0U)?EFI_SUCCESS:EFI_DEVICE_ERROR);
    }
    MicroSecondDelay(10);
  }
  return RequestDone(EFI_TIMEOUT);
}
STATIC EFI_STATUS DrainIrq(CR_SDHC *C,BOOLEAN Closing) {
  CR_INTERRUPT_GUARD G={0};EFI_STATUS S=CrInterruptGuardAcquire(&G);
  if(S!=EFI_SUCCESS)return S;
  if(Closing)mClosing=TRUE;
  if(mAccess){S=CrSdhcPowerMask(C,FALSE);if(S!=EFI_SUCCESS)goto Out;}
  if(mIrqOwned||CrInterruptIsRegistered(&mIrq)){
    S=CrUnregisterInterrupt(&mIrq);if(S!=EFI_SUCCESS)goto Out;mIrqOwned=FALSE;
  }
Out:CrInterruptGuardRelease(&G);return S;
}
STATIC EFI_STATUS StopHost(CR_SDHC *C) {
  if(!mHostMayLive||mHostStopped)return EFI_SUCCESS;
  /* A failed clock transition is not proof of a stopped inherited DMA
   * engine. Reestablish exact live access before RESET_ALL; retain every
   * remaining prerequisite if recovery itself cannot settle. */
  mAccess=FALSE;
  for(UINT32 I=0;I<2;++I){
    EFI_STATUS S;
    if(mClocks[I].Owned&&mClocks[I].Status!=EFI_SUCCESS){S=ClockOff(I);if(S!=EFI_SUCCESS)return S;}
    if(!mClocks[I].Owned){S=ClockOn(I,I?(mRate?mRate:400000):0);if(S!=EFI_SUCCESS)return S;}
  }
  mAccess=TRUE;
  EFI_STATUS S=CrSdhcEvents(C,0xffff,FALSE);if(S!=EFI_SUCCESS)return S;
  S=CrSdhcClock(C,FALSE);if(S!=EFI_SUCCESS)return S;
  mInternalSync=TRUE;S=CrSdhcReset(C,1);mInternalSync=FALSE;if(S!=EFI_SUCCESS)return S;
  mHostStopped=TRUE;return EFI_SUCCESS;
}
EFI_STATUS CrSdResourcesFreeze(CR_SDHC *C) {
  if(!C||C!=mController)return EFI_INVALID_PARAMETER;
  CR_INTERRUPT_GUARD G={0};EFI_STATUS S=CrInterruptGuardAcquire(&G);
  if(S!=EFI_SUCCESS)return S;
  mFrozen=TRUE;mClosing=TRUE;BOOLEAN Busy=mBusy;
  /* Never lower TPL for regulator restoration during EBS. Successful IRQ
   * drain retains all votes and event/code storage for the final host stop. */
  S=DrainIrq(C,TRUE);CrInterruptGuardRelease(&G);if(S!=EFI_SUCCESS)return S;
  return Busy?EFI_NOT_READY:EFI_SUCCESS;
}
EFI_STATUS CrSdResourcesRelease(CR_SDHC *C) {
  if(!C)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Enter(TRUE);if(S!=EFI_SUCCESS)return S;
  if(!mController)return Leave(EFI_SUCCESS);
  if(C!=mController)return Leave(EFI_ACCESS_DENIED);
  if(mRequestActive)return Leave(EFI_NOT_READY);
  S=DrainIrq(C,TRUE);if(S!=EFI_SUCCESS)return Leave(S);
  if(mPowerWork){S=gBS->CloseEvent(mPowerWork);if(S!=EFI_SUCCESS)return Leave(S);mPowerWork=NULL;}
  /* Startup may have failed before the adapter's RESET_ALL. Observe a stop
   * under still-live access before dropping clocks/rails or original pads. */
  S=StopHost(C);if(S!=EFI_SUCCESS)return Leave(S);
  /* No live IRQ/hardware client remains: exact reverse ownership cleanup.
   * Stop on the first failure so prerequisites survive for the same retry. */
  if(mGpioOwned){S=mGpio->ReleaseSd(mGpio,C);if(S!=EFI_SUCCESS)return Leave(S);mGpioOwned=FALSE;}
  for(UINT32 I=2;I>0;--I){S=RailOff(I-1);if(S!=EFI_SUCCESS)return Leave(S);}
  mAccess=FALSE;
  for(UINT32 I=2;I>0;--I){S=ClockOff(I-1);if(S!=EFI_SUCCESS)return Leave(S);}
  for(UINT32 I=2;I>0;--I)if(mPaths[I-1]){
    S=mIcb->ReleasePath(mIcb,mPaths[I-1]);if(S!=EFI_SUCCESS)return Leave(S);mPaths[I-1]=INTERCONNECT_INVALID_PATH;
  }
  C->SyncPower=NULL;mController=NULL;mSdTarget=NULL;mClosing=FALSE;mRate=0;mBusValid=mIoValid=FALSE;
  mHostMayLive=mHostStopped=mIrqPaused=FALSE;return Leave(EFI_SUCCESS);
}
