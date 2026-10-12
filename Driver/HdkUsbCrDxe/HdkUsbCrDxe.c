/* Native HDK8245/8350 USB2 fixed-host producer.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/HdkUsbCrLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EFIDmaIdentityCrProtocol.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/EFIUsbTypeCCrProtocol.h>
#include <Protocol/IoMmu.h>
#include <Protocol/Usb2HcFreeze.h>
#ifdef CR_HDK_USB_PRIMARY
#define SELECTED_PORT 0U
#else
#define SELECTED_PORT 1U
#endif

STATIC CR_HDK_USB mUsb;
STATIC EFI_CLOCK_CR_PROTOCOL *mClock;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmh;
STATIC EFI_GPIO_CR_PROTOCOL *mGpio;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mIcb;
STATIC EFI_SMMU_CR_PROTOCOL *mSmmu;
STATIC INTERCONNECT_PATH_HANDLE mPaths[2];
STATIC EFI_DMA_IDENTITY_CR_PROTOCOL mIdentity;
STATIC EFI_HANDLE mHost;
STATIC EFI_EVENT mExit;
STATIC BOOLEAN mStarting,mHostRegistered,mStreamAttempted,mObserver,mExiting;
STATIC BOOLEAN mEntered,mEntryBusy,mRetiring,mExitFrozen,mConnecting,mConnectAttempted,mPreparing;
#ifdef CR_HDK_USB_PRIMARY
STATIC EFI_USB_TYPEC_CR_PROTOCOL *mTypeC;
STATIC EFI_HANDLE mImage,mTypeHandle;
STATIC EFI_EVENT mMonitor;
STATIC BOOLEAN mTypeLease,mMonitorBusy;
STATIC UINT32 mRouteEpoch,mAttemptEpoch,mReportedEpoch;
STATIC EFI_STATUS AdmitHost(VOID *X) {
  CR_TYPEC_NON_PD_HOST_WITNESS W={0};
  if(X!=&mUsb||!mTypeLease||!mTypeC||!mTypeC->GetNonPdHostWitness)return EFI_NOT_READY;
  EFI_STATUS S=mTypeC->GetNonPdHostWitness(mTypeC,&W);if(S!=EFI_SUCCESS)return S;
  if(W.Revision!=CR_TYPEC_NON_PD_WITNESS_REVISION||W.Board!=mUsb.Target->Board||!W.Epoch||
    (W.Flags&CR_TYPEC_WITNESS_REQUIRED)!=CR_TYPEC_WITNESS_REQUIRED)return EFI_NOT_READY;
  if(!mRouteEpoch)mRouteEpoch=W.Epoch;
  return W.Epoch==mRouteEpoch?EFI_SUCCESS:EFI_NOT_READY;
}
#endif
STATIC EFI_STATUS Exact(EFI_STATUS S) {return S==EFI_SUCCESS || EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Access(VOID *X) {
  if(mExiting&&!mRetiring)return EFI_ACCESS_DENIED;
  if(X!=&mUsb||!mClock||!mRpmh||!mGpio||!mIcb||!mSmmu)return EFI_NOT_READY;
#ifdef CR_HDK_USB_PRIMARY
  if(!mRetiring)return AdmitHost(X);
#endif
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Floor(VOID *X) {
  if(Access(X)!=EFI_SUCCESS)return EFI_NOT_READY;
  return mRpmh->EnsureBootFloor(mRpmh);
}
STATIC EFI_STATUS Address(CR_HDK_USB_WINDOW W,UINT32 O,UINTN *A) {
  if(!A || !mUsb.Port || (O&3))return EFI_INVALID_PARAMETER;
  CONST CR_HDK_USB_PORT *P=mUsb.Port;
  if(W==HdkUsbController) {
    /* The DWC3 core and QSCRATCH wrapper are independent public DT views;
     * the core's cd00 extent is not widened to cover the wrapper. */
    if(O<0xcd00 || (O>=0xf8800 && O<=0xf8830)){*A=P->Controller+O;return EFI_SUCCESS;}
  } else if(W==HdkUsbHs && O<=P->HsPhyBytes-4){*A=P->HsPhy+O;return EFI_SUCCESS;}
  else if(W==HdkUsbEud && P->Eud && !O){*A=P->Eud;return EFI_SUCCESS;}
  return EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS Read(VOID *X,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 *V) {
  UINTN A;if(Access(X)!=EFI_SUCCESS||!V)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Address(W,O,&A);if(S==EFI_SUCCESS)*V=MmioRead32(A);return S;
}
STATIC EFI_STATUS Write(VOID *X,CR_HDK_USB_WINDOW W,UINT32 O,UINT32 V) {
  UINTN A;if(Access(X)!=EFI_SUCCESS || W==HdkUsbEud)return EFI_INVALID_PARAMETER;
  EFI_STATUS S=Address(W,O,&A);if(S==EFI_SUCCESS){MmioWrite32(A,V);MemoryFence();}return S;
}
STATIC VOID Delay(VOID *X,UINT32 N) {(VOID)X;MicroSecondDelay(N);}
STATIC EFI_STATUS Path(VOID *X,UINT32 I,BOOLEAN On,BOOLEAN *Owned) {
  if(Access(X)!=EFI_SUCCESS||I>=2||!Owned)return EFI_INVALID_PARAMETER;
  EFI_STATUS S;if(On){
    S=mIcb->AcquirePath(mIcb,mUsb.Port->IccProvider,
      I?mUsb.Port->ConfigSource:mUsb.Port->MemorySource,
      I?mUsb.Port->ConfigDestination:mUsb.Port->MemoryDestination,&mPaths[I]);
    *Owned=mPaths[I]!=INTERCONNECT_INVALID_PATH;
    /* Android high-speed vectors, expressed in the provider's kB/s units. */
    if(S==EFI_SUCCESS && *Owned){S=Access(X);if(S==EFI_SUCCESS)S=mIcb->SetBandwidth(mIcb,mPaths[I],I?0:240000,I?40000:700000);}
  } else {S=mIcb->ReleasePath(mIcb,mPaths[I]);if(S==EFI_SUCCESS){*Owned=FALSE;mPaths[I]=INTERCONNECT_INVALID_PATH;}}
  return S;
}
STATIC EFI_STATUS Domain(VOID *X,BOOLEAN On,BOOLEAN *Owned) {
  if(Access(X)!=EFI_SUCCESS||!Owned)return EFI_INVALID_PARAMETER;
  if(On)return CrAcquireGdsc(mClock,"gcc",mUsb.Port->Gdsc,Owned);
  EFI_STATUS S=mClock->SetGdsc(mClock,"gcc",mUsb.Port->Gdsc,FALSE);if(S==EFI_SUCCESS)*Owned=FALSE;return S;
}
STATIC EFI_STATUS Clock(VOID *X,CONST CHAR8 *Name,UINT64 Rate,BOOLEAN On,BOOLEAN *Owned) {
  if(Access(X)!=EFI_SUCCESS||!Owned)return EFI_INVALID_PARAMETER;
  if(On)return CrAcquireClock(mClock,"gcc",Name,Rate,Owned);
  EFI_STATUS S=mClock->SetClock(mClock,"gcc",Name,0,FALSE);if(S==EFI_SUCCESS)*Owned=FALSE;return S;
}
STATIC EFI_STATUS Rail(VOID *X,CONST CR_HDK_USB_RAIL *R,BOOLEAN On,BOOLEAN *Owned) {
  if(Access(X)!=EFI_SUCCESS||!R||!Owned)return EFI_INVALID_PARAMETER;
  if(On)return mRpmh->AcquireVreg(mRpmh,R->Name,R->Millivolts,R->Mode,Owned);
  EFI_STATUS S=mRpmh->RpmhEnableVreg(mRpmh,R->Name,FALSE);if(S==EFI_SUCCESS)*Owned=FALSE;return S;
}
STATIC EFI_STATUS Reset(VOID *X,CONST CHAR8 *Name,BOOLEAN Assert) {
  if(Access(X)!=EFI_SUCCESS)return EFI_NOT_READY;
  return mClock->SetReset(mClock,"gcc",Name,Assert);
}
STATIC EFI_STATUS Gpio(VOID *X,UINT32 I,BOOLEAN On,BOOLEAN *Owned) {
  if(Access(X)!=EFI_SUCCESS||I>=2||!Owned)return EFI_INVALID_PARAMETER;
  UINT16 Pin=I?mUsb.Port->HubGpio:mUsb.Port->VbusGpio;
  UINT8 Value=I?mUsb.Port->HubValue:mUsb.Port->VbusValue;
  if(Pin==CR_HDK_USB_NO_GPIO)return EFI_INVALID_PARAMETER;
  EFI_STATUS S;if(On){
    GpioConfigParams Config={Pin,TRUE,GPIO_FUNC_NORMAL,GPIO_PULL_NONE,GPIO_DRIVE_STRENGTH_2MA,(GpioValueType)!Value};
    S=mGpio->AcquirePin(mGpio,&mUsb,&Config,Owned);if(S!=EFI_SUCCESS)return S;
    if(!*Owned){mUsb.UncertainOwner=TRUE;return EFI_NOT_READY;}
    S=Access(X);if(S!=EFI_SUCCESS)return S;
    S=mGpio->SetPinValue(mGpio,&mUsb,Pin,Value);if(S!=EFI_SUCCESS)return S;
    S=Access(X);if(S!=EFI_SUCCESS)return S;
    UINT8 Got;S=mGpio->ReadPinValue(mGpio,&mUsb,Pin,&Got);if(S==EFI_SUCCESS&&Got!=Value)S=EFI_DEVICE_ERROR;
  } else {S=mGpio->ReleasePin(mGpio,&mUsb,Pin);if(S==EFI_SUCCESS)*Owned=FALSE;}
  return S;
}
STATIC EFI_STATUS EFIAPI VerifyStopped(VOID *X) {
  if(X!=&mUsb || !mStarting || mHostRegistered || mExiting)return EFI_NOT_READY;
  return CrHdkUsbVerifyStopped(&mUsb);
}
STATIC EFI_STATUS Attach(VOID *X) {
  if(Access(X)!=EFI_SUCCESS||!mStarting||mHostRegistered||mExiting)return EFI_NOT_READY;
  /* The provider can retain a private context after a failed route commit.
   * There is no per-SID detach API. Never unload or release its prerequisites
   * after attempting this operation, including on failed initialization. */
  mStreamAttempted=TRUE;
  return mSmmu->AcquireUsbStream(mSmmu,mUsb.Port->Controller,mUsb.Port->StreamId,VerifyStopped,&mUsb);
}
STATIC CONST CR_HDK_USB_IO mIo={&mUsb,Access,Floor,Read,Write,Delay,Path,Domain,Clock,Rail,Reset,Gpio,Attach,
#ifdef CR_HDK_USB_PRIMARY
  AdmitHost
#else
  NULL
#endif
};
STATIC EFI_STATUS EFIAPI HostInit(NON_DISCOVERABLE_DEVICE *Device) {
  (VOID)Device;return !mExiting && mUsb.Ready && Access(&mUsb)==EFI_SUCCESS?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS PrepareExitImpl(VOID *X) {
  if(X!=&mUsb)return EFI_INVALID_PARAMETER;mExiting=TRUE;
  if(mExitFrozen)return EFI_SUCCESS;
  /* Close admission even if EBS arrived reentrantly inside a provider/child
   * operation. Its returned ownership journal still belongs to this image;
   * no MMIO or release is safe until that operation has returned. */
  if(mStarting||mConnecting){mUsb.Closed=TRUE;return EFI_NOT_READY;}
  /* This is registered with the SMMU's pre-revocation pass. Close and drain
   * the exact child before touching its running hardware. No allocations,
   * disconnect, unmap or protocol lifetime changes are made during EBS. */
  if(mHostRegistered){
    EDKII_USB2_HC_FREEZE_PROTOCOL *F=NULL;EFI_USB2_HC_PROTOCOL *H=NULL;UINT32 Active=0;
    EFI_STATUS Fs=Exact(gBS->HandleProtocol(mHost,&gEdkiiUsb2HcFreezeProtocolGuid,(VOID **)&F));
    EFI_STATUS Hs=Exact(gBS->HandleProtocol(mHost,&gEfiUsb2HcProtocolGuid,(VOID **)&H));
    if(Fs==EFI_NOT_FOUND&&Hs==EFI_NOT_FOUND&&!mConnectAttempted)goto HaltOnly;
    if(Fs!=EFI_SUCCESS)return Fs;if(Hs!=EFI_SUCCESS)return Hs;
    if(!F||F->Revision!=EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION||!F->Freeze||F->HostController!=H)return EFI_UNSUPPORTED;
    EFI_STATUS S=Exact(F->Freeze(F,&Active));if(S!=EFI_SUCCESS||Active)return S==EFI_SUCCESS?EFI_NOT_READY:S;
  }
HaltOnly:
  mRetiring=TRUE;EFI_STATUS S=CrHdkUsbFreeze(&mUsb);mRetiring=FALSE;
  if(S==EFI_SUCCESS)mExitFrozen=TRUE;
  DEBUG((DEBUG_WARN,"Dwc3Cr: USB%u permanent freeze/halt=%r owners retained\n",SELECTED_PORT,S));return S;
}
STATIC EFI_STATUS EFIAPI PrepareExit(VOID *X) {
  if(X!=&mUsb)return EFI_INVALID_PARAMETER;mExiting=TRUE;
  if(mExitFrozen)return EFI_SUCCESS;
  if(mPreparing)return EFI_NOT_READY;
  mPreparing=TRUE;EFI_STATUS S=PrepareExitImpl(X);mPreparing=FALSE;return S;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *X) {(VOID)Event;(VOID)PrepareExit(X);}
STATIC EFI_STATUS StartHost(VOID) {
  EFI_STATUS S;mStarting=TRUE;S=CrHdkUsbStart(&mUsb);mStarting=FALSE;
  if(S!=EFI_SUCCESS)return S;if(mExiting)return EFI_ACCESS_DENIED;
  mIdentity=(EFI_DMA_IDENTITY_CR_PROTOCOL){EFI_DMA_IDENTITY_CR_PROTOCOL_REVISION,0x15000000,mUsb.Port->Controller,0xcd00,mUsb.Port->StreamId};
  S=Exact(gBS->InstallMultipleProtocolInterfaces(&mHost,&gEfiDmaIdentityCrProtocolGuid,&mIdentity,NULL));if(S!=EFI_SUCCESS)return S;
  if(mExiting)return EFI_ACCESS_DENIED;
  S=Exact(RegisterNonDiscoverableMmioDevice(SELECTED_PORT?0xd301:0xd300,NonDiscoverableDeviceTypeXhci,NonDiscoverableDeviceDmaTypeNonCoherent,HostInit,&mHost,
    1,(UINTN)mUsb.Port->Controller,(UINTN)0xc000));if(S!=EFI_SUCCESS)return S;mHostRegistered=TRUE;
  if(mExiting)return EFI_ACCESS_DENIED;
  mConnectAttempted=mConnecting=TRUE;S=Exact(gBS->ConnectController(mHost,NULL,NULL,TRUE));mConnecting=FALSE;
  if(mExiting)return EFI_ACCESS_DENIED;
  DEBUG((DEBUG_WARN,"Dwc3Cr: USB%u USB2 host core=%08x rev=%08x SID=%x xHCI-connect=%r\n",SELECTED_PORT,mUsb.Id,mUsb.Revision,mUsb.Port->StreamId,S));
  return EFI_SUCCESS;
}
#ifdef CR_HDK_USB_PRIMARY
STATIC VOID EFIAPI Monitor(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(mExiting||mStarting||mMonitorBusy||!mTypeLease)return;mMonitorBusy=TRUE;
  CR_TYPEC_NON_PD_HOST_WITNESS W={0};EFI_STATUS S=Exact(mTypeC->GetNonPdHostWitness(mTypeC,&W));
  if(mUsb.Ready){
    if(S!=EFI_SUCCESS||W.Epoch!=mRouteEpoch||W.Board!=mUsb.Target->Board||
      (W.Flags&CR_TYPEC_WITNESS_REQUIRED)!=CR_TYPEC_WITNESS_REQUIRED){
      EFI_STATUS Halt=PrepareExit(&mUsb);
      DEBUG((DEBUG_WARN,"Dwc3Cr: USB0 non-PD epoch changed %u->%u flags=%x state=%r freeze/halt=%r; restart unavailable, owners retained\n",mRouteEpoch,W.Epoch,W.Flags,S,Halt));
    }
  }else if(!mStreamAttempted && !mUsb.Stage && W.Epoch && W.Epoch!=mAttemptEpoch){
    if(W.Epoch!=mReportedEpoch){mReportedEpoch=W.Epoch;DEBUG((DEBUG_WARN,"Dwc3Cr: USB0 non-PD witness epoch=%u flags=%x status=%r; PD policy remains unavailable\n",W.Epoch,W.Flags,S));}
    if(S==EFI_SUCCESS && W.Board==mUsb.Target->Board && (W.Flags&CR_TYPEC_WITNESS_REQUIRED)==CR_TYPEC_WITNESS_REQUIRED){
      mAttemptEpoch=W.Epoch;mRouteEpoch=0;S=StartHost();
      if(S!=EFI_SUCCESS){
        EFI_STATUS Cleanup=EFI_ACCESS_DENIED;
        /* Before stream adoption or publication, a lost route may retire
         * only this failed cold attempt. Its owned prerequisites are still
         * accessible for reset/release; host admission remains closed. */
        if(!mStreamAttempted&&!mHostRegistered&&!mExiting){
          mRetiring=TRUE;Cleanup=CrHdkUsbRelease(&mUsb);mRetiring=FALSE;
        }
        DEBUG((DEBUG_ERROR,"Dwc3Cr: USB0 non-PD start=%r stage=%u window=%u reg=%x value=%x cleanup=%r epoch=%u\n",S,mUsb.Stage,mUsb.LastWindow,mUsb.LastOffset,mUsb.LastValue,Cleanup,W.Epoch));
      }
    }
  }
  mMonitorBusy=FALSE;
}
STATIC EFI_STATUS OpenTypeC(VOID) {
  EFI_HANDLE *Handles=NULL;UINTN Count=0;
  EFI_STATUS S=Exact(gBS->LocateHandleBuffer(ByProtocol,&gEfiUsbTypeCCrProtocolGuid,NULL,&Count,&Handles));
  if(mExiting)return EFI_ACCESS_DENIED;
  if(S!=EFI_SUCCESS)return S;
  if(Count!=1){FreePool(Handles);return mExiting?EFI_ACCESS_DENIED:EFI_UNSUPPORTED;}
  mTypeHandle=Handles[0];FreePool(Handles);if(mExiting)return EFI_ACCESS_DENIED;
  S=Exact(gBS->OpenProtocol(mTypeHandle,&gEfiUsbTypeCCrProtocolGuid,(VOID **)&mTypeC,mImage,mTypeHandle,EFI_OPEN_PROTOCOL_BY_DRIVER));
  if(S==EFI_SUCCESS)mTypeLease=TRUE;if(mExiting)return EFI_ACCESS_DENIED;if(S!=EFI_SUCCESS)return S;
  if(!mTypeC||mTypeC->Revision<EFI_USB_TYPEC_NON_PD_WITNESS_REVISION||!mTypeC->GetNonPdHostWitness)return EFI_UNSUPPORTED;
  S=Exact(gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Monitor,&mUsb,&mMonitor));if(mExiting)return EFI_ACCESS_DENIED;if(S!=EFI_SUCCESS)return S;
  S=Exact(gBS->SetTimer(mMonitor,TimerPeriodic,100000));if(mExiting)return EFI_ACCESS_DENIED;if(S!=EFI_SUCCESS)return S;
  Monitor(NULL,&mUsb);return EFI_SUCCESS;
}
#endif
EFI_STATUS EFIAPI HdkUsbCrUnload(EFI_HANDLE Image) {
  (VOID)Image;
  /* Callback, DMA identity and native stream owners have permanent storage.
   * Normal image unload is available only before stream/publication starts. */
  if(mEntryBusy||mStreamAttempted||mHostRegistered||mObserver||mExiting)return EFI_ACCESS_DENIED;
  if(!mUsb.Initialized)return EFI_SUCCESS;return CrHdkUsbRelease(&mUsb);
}
STATIC EFI_STATUS Entry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  (VOID)Image;(VOID)System;EFI_STATUS S,Cleanup;
  if(mUsb.Initialized)return EFI_ALREADY_STARTED;
#define LOCATE(G,P) do{S=Exact(gBS->LocateProtocol(&(G),NULL,(VOID **)&(P)));if(S!=EFI_SUCCESS)return S;if(mExiting)return EFI_ACCESS_DENIED;}while(0)
  LOCATE(gEfiClockCrProtocolGuid,mClock);LOCATE(gEfiRpmhCrProtocolGuid,mRpmh);
  LOCATE(gEfiGpioCrProtocolGuid,mGpio);LOCATE(gEfiInterconnectCrProtocolGuid,mIcb);LOCATE(gEfiSmmuCrProtocolGuid,mSmmu);
#undef LOCATE
  if(!mClock||mClock->Revision<EFI_CLOCK_CR_PROTOCOL_REVISION||!mClock->AcquireClock||!mClock->AcquireGdsc||!mClock->SetClock||!mClock->SetGdsc||!mClock->SetReset||
    !mRpmh||mRpmh->Revision<EFI_RPMH_CR_PROTOCOL_REVISION||!mRpmh->EnsureBootFloor||!mRpmh->AcquireVreg||!mRpmh->RpmhEnableVreg||
    !mGpio||mGpio->Revision<0x10002||!mGpio->AcquirePin||!mGpio->SetPinValue||!mGpio->ReadPinValue||!mGpio->ReleasePin||
    !mIcb||!mIcb->AcquirePath||!mIcb->SetBandwidth||!mIcb->ReleasePath||
    !mSmmu||mSmmu->Revision<0x1000a||!mSmmu->AcquireUsbStream||!mSmmu->SetExitPrepareObserver)return EFI_UNSUPPORTED;
  CONST CR_HDK_USB_TARGET *Target=CrHdkUsbGetTarget();
  S=CrHdkUsbInit(&mUsb,Target,SELECTED_PORT,&mIo);if(S!=EFI_SUCCESS)return S;
  /* Fence EBS before source/provider acquisition, stream adoption or child
   * publication. The permanent SMMU observer is sufficient if event creation
   * subsequently fails. Every returned callback is checked for closed admission. */
  S=Exact(mSmmu->SetExitPrepareObserver(mSmmu,&mUsb,PrepareExit,&mUsb));if(S!=EFI_SUCCESS)goto Failure;mObserver=TRUE;
  if(mExiting){S=EFI_ACCESS_DENIED;goto Failure;}
  S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,&mUsb,&gEfiEventExitBootServicesGuid,&mExit));if(S!=EFI_SUCCESS)goto Failure;
  if(mExiting){S=EFI_ACCESS_DENIED;goto Failure;}
#ifdef CR_HDK_USB_PRIMARY
  S=OpenTypeC();if(S!=EFI_SUCCESS)goto Failure;return EFI_SUCCESS;
#else
  /* Primary has a different policy owner. Keep its EUD/PD/GLINK state visible
   * without using a CC observer as authority for negotiated USB/DP routing. */
  EFI_USB_TYPEC_CR_PROTOCOL *TypeC=NULL;EFI_USB_TYPEC_STATE State={0};
  S=gBS->LocateProtocol(&gEfiUsbTypeCCrProtocolGuid,NULL,(VOID **)&TypeC);
  if(mExiting){S=EFI_ACCESS_DENIED;goto Failure;}
  if(S==EFI_SUCCESS && TypeC && TypeC->Revision>=EFI_USB_TYPEC_CR_PROTOCOL_REVISION && TypeC->GetState)S=TypeC->GetState(TypeC,&State);
  if(mExiting){S=EFI_ACCESS_DENIED;goto Failure;}
  DEBUG((DEBUG_WARN,"Dwc3Cr: USB0 passive TypeC=%r connected=%u role=%u PD=%u; policy owner required\n",S,State.Connected,State.DataRole,State.PdReady));
  S=StartHost();if(S!=EFI_SUCCESS)goto Failure;
  /* BDS may bind a published host later; a child connect failure retains the
   * producer, translation and all prerequisites for review/recovery. */
  return EFI_SUCCESS;
#endif
Failure:
  Cleanup=mStreamAttempted||mObserver||mHostRegistered||mExiting?EFI_ACCESS_DENIED:CrHdkUsbRelease(&mUsb);
  DEBUG((DEBUG_ERROR,"Dwc3Cr: USB%u start=%r stage=%u window=%u reg=%x value=%x retained-cleanup=%r stream-attempt=%u\n",
    SELECTED_PORT,S,mUsb.Stage,mUsb.LastWindow,mUsb.LastOffset,mUsb.LastValue,Cleanup,mStreamAttempted));
  /* A failed cleanup or stream attempt keeps this DXE image resident, never
   * leaves callbacks or owner identities pointing into a freed image. */
  return Cleanup==EFI_SUCCESS?S:EFI_SUCCESS;
}
EFI_STATUS EFIAPI HdkUsbCrEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System) {
  if(mEntered||mEntryBusy)return EFI_ALREADY_STARTED;
#ifdef CR_HDK_USB_PRIMARY
  mImage=Image;
#endif
  mEntered=mEntryBusy=TRUE;EFI_STATUS S=Entry(Image,System);mEntryBusy=FALSE;return S;
}
