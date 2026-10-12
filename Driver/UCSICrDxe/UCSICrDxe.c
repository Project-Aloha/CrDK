/** Board-selected native UCSI/PMIC GLink monitor. SPDX-License-Identifier: GPL-2.0-only */
#include <Library/UcsiCrLib.h>
#include <Library/UcsiTargetLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIPilCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFISmemCrProtocol.h>
#include <Protocol/EFIUsbTypeCCrProtocol.h>
#include <Protocol/EFIPmicGlinkCrProtocol.h>
#include <Guid/EventGroup.h>

STATIC CR_GLINK mLink;
STATIC CR_PMIC_GLINK mPmic;
STATIC CR_QRTR mQrtr;
STATIC CR_CHARGER_LOG mChargerLog;
STATIC CR_IPCC mIpcc;
STATIC CR_GLINK_SMEM mSmemOps;
STATIC EFI_USB_TYPEC_CR_PROTOCOL mProtocol;
STATIC EFI_SMEM_CR_PROTOCOL *mSmem;
STATIC EFI_GPIO_CR_PROTOCOL *mGpio;
STATIC EFI_PIL_CR_PROTOCOL *mPilLoader;
STATIC EFI_PIL_CR_PROTOCOL *mPilOwner;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmh;
STATIC BOOLEAN mPdSupplyOwned;
STATIC CONST CR_UCSI_TARGET *mTarget;
STATIC BOOLEAN mOrientationOwned;
STATIC BOOLEAN mPilSeenReady, mRebind;
STATIC EFI_EVENT mTimer, mExit, mWork;
STATIC BOOLEAN mStopped, mPeerReady;
STATIC BOOLEAN mServicing, mRxPending, mIrqLogged;
STATIC BOOLEAN mPublished;
/* Exit is one-way. Failed ownership/event retirement pins this image, and
   no callback may admit new work while entry still holds a provider call. */
STATIC BOOLEAN mExiting, mEntryBusy, mOwnershipUncertain;
STATIC UINT32 mPeerGeneration;
STATIC UINT32 mPdGeneration;
STATIC UINT32 mLoggedSequence;
STATIC UINT64 mStartedUs;
STATIC BOOLEAN mWaitLogged;
/* All PMIC clients share this one GLink/IPCC/PIL owner. Fixed storage remains
   resident while registered subscribers retain callback pointers. */
#define PMIC_SUBSCRIBERS 4U
STATIC CR_STATUS Wake(VOID *Context);
STATIC EFI_PMIC_GLINK_CR_PROTOCOL mPmicProtocol;
STATIC EFI_PMIC_GLINK_CR_STATE mPmicTransport;
STATIC UINT32 mAdspGeneration, mTransportGeneration;
STATIC BOOLEAN mDemuxDispatch;
typedef struct {
  UINT32 Owner;
  VOID *Identity, *Context;
  EFI_PMIC_GLINK_RECEIVE Receive;
  EFI_PMIC_GLINK_STATE_CHANGE State;
} PMIC_SUBSCRIBER;
STATIC PMIC_SUBSCRIBER mSubscribers[PMIC_SUBSCRIBERS];
STATIC UINT32 PacketOwner(CONST VOID *Data)
{
  CONST UINT8 *P=Data;
  return (UINT32)P[0] | ((UINT32)P[1]<<8) | ((UINT32)P[2]<<16) | ((UINT32)P[3]<<24);
}
STATIC VOID PublishPmicState(BOOLEAN Online, EFI_STATUS Status, BOOLEAN NewPeer)
{
  EFI_PMIC_GLINK_CR_STATE State;
  UINTN I; EFI_TPL Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  if(mStopped || mExiting) Online=FALSE;
  if(NewPeer) ++mTransportGeneration;
  ZeroMem(&State,sizeof(State));
  State.Online=Online; State.AdspGeneration=mAdspGeneration;
  State.PeerGeneration=mTransportGeneration; State.LastError=Status;
  if(CompareMem(&State,&mPmicTransport,sizeof(State))==0) { gBS->RestoreTPL(Tpl); return; }
  mPmicTransport=State;
  mDemuxDispatch=TRUE;
  for(I=0;I<PMIC_SUBSCRIBERS;++I)
    if(mSubscribers[I].Identity!=NULL)
      mSubscribers[I].State(mSubscribers[I].Context,&mPmicTransport);
  mDemuxDispatch=FALSE;
  gBS->RestoreTPL(Tpl);
}
STATIC EFI_STATUS EFIAPI PmicRegister(EFI_PMIC_GLINK_CR_PROTOCOL *This,
  UINT32 Owner, VOID *Identity, EFI_PMIC_GLINK_RECEIVE ReceiveCallback,
  EFI_PMIC_GLINK_STATE_CHANGE StateCallback, VOID *Context)
{
  UINTN I, Free=PMIC_SUBSCRIBERS; EFI_TPL Tpl;
  if(This!=&mPmicProtocol || Identity==NULL || ReceiveCallback==NULL || StateCallback==NULL)
    return EFI_INVALID_PARAMETER;
  if(mExiting || mEntryBusy)return EFI_NOT_READY;
  /* The public native client currently qualifies only Android's battery
     owner. UCSI/PAN are already owned by this driver's built-in engine. */
  if(Owner!=32778U) return EFI_UNSUPPORTED;
  Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  if(mStopped || !mPublished || mDemuxDispatch) { gBS->RestoreTPL(Tpl); return EFI_NOT_READY; }
  for(I=0;I<PMIC_SUBSCRIBERS;++I) {
    if(mSubscribers[I].Identity==NULL) { if(Free==PMIC_SUBSCRIBERS) Free=I; continue; }
    if(mSubscribers[I].Owner==Owner) { gBS->RestoreTPL(Tpl); return EFI_ALREADY_STARTED; }
  }
  if(Free==PMIC_SUBSCRIBERS) { gBS->RestoreTPL(Tpl); return EFI_OUT_OF_RESOURCES; }
  mSubscribers[Free].Owner=Owner; mSubscribers[Free].Identity=Identity;
  mSubscribers[Free].Receive=ReceiveCallback; mSubscribers[Free].State=StateCallback;
  mSubscribers[Free].Context=Context;
  mDemuxDispatch=TRUE; StateCallback(Context,&mPmicTransport); mDemuxDispatch=FALSE;
  gBS->RestoreTPL(Tpl); return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI PmicUnregister(EFI_PMIC_GLINK_CR_PROTOCOL *This,
                                       UINT32 Owner, VOID *Identity)
{
  UINTN I; EFI_TPL Tpl;
  if(This!=&mPmicProtocol || Identity==NULL) return EFI_INVALID_PARAMETER;
  if(mExiting || mEntryBusy)return EFI_NOT_READY;
  Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  if(mDemuxDispatch) { gBS->RestoreTPL(Tpl); return EFI_NOT_READY; }
  for(I=0;I<PMIC_SUBSCRIBERS;++I) if(mSubscribers[I].Owner==Owner && mSubscribers[I].Identity!=NULL) {
    if(mSubscribers[I].Identity!=Identity) { gBS->RestoreTPL(Tpl); return EFI_ACCESS_DENIED; }
    ZeroMem(&mSubscribers[I],sizeof(mSubscribers[I]));
    gBS->RestoreTPL(Tpl); return EFI_SUCCESS;
  }
  gBS->RestoreTPL(Tpl); return EFI_NOT_FOUND;
}
STATIC EFI_STATUS EFIAPI PmicSend(EFI_PMIC_GLINK_CR_PROTOCOL *This,
  UINT32 Owner, VOID *Identity, CONST VOID *Data, UINTN Length)
{
  UINTN I; EFI_STATUS Status=EFI_ACCESS_DENIED; EFI_TPL Tpl;
  if(This!=&mPmicProtocol || Identity==NULL || Data==NULL || Length<12 || PacketOwner(Data)!=Owner)
    return EFI_INVALID_PARAMETER;
  if(mExiting || mEntryBusy)return EFI_NOT_READY;
  Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  if(mStopped || !mPmicTransport.Online || mDemuxDispatch || !mPeerReady ||
     mPeerGeneration!=mLink.Generation) Status=EFI_NOT_READY;
  else for(I=0;I<PMIC_SUBSCRIBERS;++I)
    if(mSubscribers[I].Owner==Owner && mSubscribers[I].Identity==Identity) {
      Status=CrGlinkSend(&mLink,Data,Length);
      /* Queue success is acceptance. A work-signal error cannot revoke the
         accepted packet; the existing timer also drains the TX queue. */
      if(Status==EFI_SUCCESS) (VOID)Wake(NULL);
      break;
    }
  gBS->RestoreTPL(Tpl); return Status;
}
STATIC EFI_STATUS EFIAPI PmicGetState(EFI_PMIC_GLINK_CR_PROTOCOL *This,
                                     EFI_PMIC_GLINK_CR_STATE *State)
{
  EFI_TPL Tpl;
  if(This!=&mPmicProtocol || State==NULL) return EFI_INVALID_PARAMETER;
  if(mExiting){*State=mPmicTransport;State->Online=FALSE;return EFI_NOT_READY;}
  Tpl=gBS->RaiseTPL(TPL_NOTIFY); *State=mPmicTransport; gBS->RestoreTPL(Tpl);
  return State->Online ? EFI_SUCCESS : EFI_NOT_READY;
}

STATIC EFI_STATUS StopMonitor(VOID);
STATIC CR_STATUS QuiescePil(VOID *Context);
STATIC CR_STATUS Wake(VOID *Context)
{
  (VOID)Context;
  /* Startup can receive an IRQ before publication. The latched Pending bit
     is drained by the first timer; accepting it here avoids poisoning IPCC
     merely because deferred work is still fenced by the entry owner. */
  if(!mExiting&&mEntryBusy&&mWork!=NULL)return EFI_SUCCESS;
  if(mStopped || mExiting || mEntryBusy || mWork==NULL)return EFI_NOT_READY;
  return gBS->SignalEvent(mWork);
}
STATIC CR_STATUS SmemGet(VOID *Context, UINT32 Host, UINT32 Item, UINT32 *Size, VOID **Address)
{
  EFI_SMEM_CR_PROTOCOL *Smem = Context;
  return Smem->GetItem(Smem, Host, Item, Size, Address);
}
STATIC CR_STATUS SmemAllocate(VOID *Context, UINT32 Host, UINT32 Item, UINT32 Size, VOID **Address)
{
  EFI_SMEM_CR_PROTOCOL *Smem = Context;
  return Smem->AllocateItem(Smem, Host, Item, Size, Address);
}

STATIC VOID Kick(VOID *Context)
{
  (VOID)Context;
  if(!mStopped&&!mExiting&&!mEntryBusy&&mTarget!=NULL)CrIpccSend(mTarget->IpccBase);
}
STATIC VOID Receive(VOID *Context, CONST UINT8 *Data, UINTN Length)
{
  (VOID)Context;
  if (!mStopped && !mExiting && !mEntryBusy && mPeerReady && mPeerGeneration == mLink.Generation) {
    UINTN I; EFI_TPL Tpl;
    CrPmicReceive(&mPmic, Data, Length);
    if(Data==NULL || Length<12) return;
    Tpl=gBS->RaiseTPL(TPL_NOTIFY);
    if(mStopped || !mPmicTransport.Online) { gBS->RestoreTPL(Tpl); return; }
    mDemuxDispatch=TRUE;
    for(I=0;I<PMIC_SUBSCRIBERS;++I)
      if(mSubscribers[I].Identity!=NULL && mSubscribers[I].Owner==PacketOwner(Data))
        mSubscribers[I].Receive(mSubscribers[I].Context,Data,Length);
    mDemuxDispatch=FALSE;
    gBS->RestoreTPL(Tpl);
  }
}
STATIC EFI_STATUS Send(VOID *Context, CONST VOID *Data, UINTN Length)
{ (VOID)Context; return CrGlinkSend(&mLink, Data, Length); }

STATIC EFI_STATUS SendQrtr(VOID *Context, CONST VOID *Data, UINTN Length)
{ (VOID)Context; return CrGlinkSendQrtr(&mLink, Data, Length); }
STATIC EFI_STATUS ReceiveQrtr(VOID *Context, CONST UINT8 *Data, UINTN Length)
{ (VOID)Context; return CrQrtrReceive(&mQrtr, Data, Length); }
STATIC VOID QrtrState(VOID *Context, BOOLEAN Online)
{ (VOID)Context; CrQrtrOnline(&mQrtr, Online); }
STATIC EFI_STATUS SendLogs(VOID *Context, CONST VOID *Data, UINTN Length)
{ (VOID)Context; return CrGlinkSendLogs(&mLink, Data, Length); }
STATIC EFI_STATUS ReceiveLogs(VOID *Context, CONST UINT8 *Data, UINTN Length)
{ (VOID)Context; return CrChargerLogReceive(&mChargerLog, Data, Length); }
STATIC VOID LogsState(VOID *Context, BOOLEAN Online)
{ (VOID)Context; CrChargerLogOnline(&mChargerLog, Online); }

STATIC BOOLEAN PilReady(VOID)
{
  EFI_PIL_CR_STATE State;
  EFI_PIL_CR_PROTOCOL *Loader = NULL;
  EFI_STATUS Status;
  if(mStopped || mExiting || mEntryBusy)return FALSE;
  /* PIL can be unloaded from Shell. Refresh discovery before dereferencing
     its status interface so a removed image never leaves a callable pointer. */
  Status = gBS->LocateProtocol(&gEfiPilCrProtocolGuid, NULL, (VOID **)&Loader);
  if(mStopped || mExiting || mEntryBusy)return FALSE;
  mPilLoader = EFI_ERROR(Status) ? NULL : Loader;
  if (mPilLoader == NULL || mPilLoader->Revision < EFI_PIL_CR_PROTOCOL_REVISION ||
      mPilLoader->GetState == NULL || EFI_ERROR(mPilLoader->GetState(mPilLoader, &State)) ||
      !State.Ready || State.Exiting) {
    if (mPilSeenReady) {
      UINT32 Sequence = mPmic.State.Sequence;
      CrPmicInitialize(&mPmic, Send, NULL);
      mPmic.State.Sequence = Sequence + 1U;
      mPeerReady = mPilSeenReady = FALSE;
      PublishPmicState(FALSE, EFI_NOT_READY, TRUE);
      mRebind = TRUE;
    }
    return FALSE;
  }
  if(mStopped || mExiting || mEntryBusy)return FALSE;
  if(!mPilSeenReady) ++mAdspGeneration;
  mPilSeenReady = TRUE;
  return TRUE;
}

STATIC EFI_STATUS EFIAPI GetState(EFI_USB_TYPEC_CR_PROTOCOL *This, EFI_USB_TYPEC_STATE *State)
{
  EFI_TPL Tpl;
  if (This != &mProtocol || State == NULL) return EFI_INVALID_PARAMETER;
  if(mStopped || mExiting || mEntryBusy){*State=mPmic.State;State->PdReady=FALSE;return EFI_NOT_READY;}
  Tpl = gBS->RaiseTPL(TPL_CALLBACK);
  BOOLEAN Ready = PilReady();
  *State = mPmic.State;
  gBS->RestoreTPL(Tpl);
  return mStopped || !Ready ? EFI_NOT_READY : EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetDataRole(EFI_USB_TYPEC_CR_PROTOCOL *This, EFI_USB_TYPEC_DATA_ROLE Role)
{
  EFI_STATUS Status;
  EFI_TPL Tpl;
  if (This != &mProtocol || (Role != UsbTypeCHost && Role != UsbTypeCDevice))
    return EFI_INVALID_PARAMETER;
  if (mStopped || mExiting || mEntryBusy) return EFI_NOT_READY;
  Tpl = gBS->RaiseTPL(TPL_CALLBACK);
  Status = PilReady() ? CrPmicRequestRole(&mPmic, FALSE, Role == UsbTypeCHost ? 1U : 2U) : EFI_NOT_READY;
  if (!EFI_ERROR(Status)) {
    Status = Wake(NULL);
    if (EFI_ERROR(Status)) mPmic.RequestedCommand = 0;
  }
  gBS->RestoreTPL(Tpl);
  return Status;
}
STATIC EFI_STATUS EFIAPI SetPowerRole(EFI_USB_TYPEC_CR_PROTOCOL *This, BOOLEAN Source)
{
  EFI_STATUS Status;
  EFI_TPL Tpl;
  if (This != &mProtocol) return EFI_INVALID_PARAMETER;
  if (mStopped || mExiting || mEntryBusy) return EFI_NOT_READY;
  Tpl = gBS->RaiseTPL(TPL_CALLBACK);
  Status = PilReady() ? CrPmicRequestRole(&mPmic, TRUE, Source ? 1U : 2U) : EFI_NOT_READY;
  if (!EFI_ERROR(Status)) {
    Status = Wake(NULL);
    if (EFI_ERROR(Status)) mPmic.RequestedCommand = 0;
  }
  gBS->RestoreTPL(Tpl);
  return Status;
}

STATIC VOID EFIAPI Service(EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  UINT64 NowUs;
  BOOLEAN Received;
  (VOID)Event; (VOID)Context;
  if (mStopped || mExiting || mEntryBusy || mServicing) return;
  mServicing = TRUE;
  NowUs = GetTimeInNanoSecond(GetPerformanceCounter()) / 1000ULL;
  Status = CrIpccService(&mIpcc, &Received);
  if(mStopped || mExiting)goto Report;
  if (EFI_ERROR(Status) && Status != EFI_NOT_READY) goto TransportFault;
  if (Received) {
    mRxPending = TRUE;
    if (!mIrqLogged) {
      DEBUG((DEBUG_WARN, "UCSICr: IPCC IRQ receive active source=%u LPASS=3 signal=0\n", CR_IPCC_SOURCE));
      mIrqLogged = TRUE;
    }
  }
  if (!PilReady()) {
    if (!mWaitLogged && NowUs - mStartedUs >= 5000000ULL) {
      DEBUG((DEBUG_WARN, "UCSICr: waiting for PILCr ADSP startup before PMIC router negotiation\n"));
      mWaitLogged = TRUE;
    }
    goto Report;
  }
  if (mRebind) {
    CrGlinkReset(&mLink);
    Status = CrGlinkBind(&mLink, &mSmemOps);
    if(mStopped || mExiting)goto Report;
    if (EFI_ERROR(Status)) goto TransportFault;
    mRebind = FALSE;
    mRxPending = TRUE; /* inspect a pre-existing FIFO once after ADSP restart */
  }
  Status = CrGlinkServiceTx(&mLink);
  if(mStopped || mExiting)goto Report;
  if (EFI_ERROR(Status) && Status != EFI_NOT_READY) goto TransportFault;
  if (mRxPending) {
    Status = CrGlinkPoll(&mLink);
    if(mStopped || mExiting)goto Report;
    if (EFI_ERROR(Status) && Status != EFI_NOT_READY) goto TransportFault;
    /* Continue an already signalled burst or partial packet, bounded to the
       transport's packet budget. Idle timer service does not inspect RX. */
    mRxPending = Status == EFI_NOT_READY ||
      (mLink.Descriptor != NULL && mLink.Descriptor[2] != mLink.Descriptor[3]);
  }
  /* RPMSG and PDR are independent: a known DOWN charger PD must not start
     UCSI, and a new UP notification restarts negotiation after a PD reset.
     Firmware without the notifier retains the Android RPMSG startup path. */
  BOOLEAN PeerReady = mLink.Ready && (!mQrtr.PdKnown || mQrtr.PdReady);
  if (PeerReady != mPeerReady || mLink.Generation != mPeerGeneration ||
      mQrtr.PdGeneration != mPdGeneration) {
    UINT32 Sequence = mPmic.State.Sequence;
    CrPmicInitialize(&mPmic, Send, NULL);
    mPmic.State.Sequence = Sequence + 1U;
    mPeerReady = PeerReady;
    mPeerGeneration = mLink.Generation;
    mPdGeneration = mQrtr.PdGeneration;
    PublishPmicState(mPeerReady, mPeerReady ? EFI_SUCCESS : EFI_NOT_READY, TRUE);
    if(mStopped || mExiting)goto Report;
    mWaitLogged = FALSE;
    mStartedUs = NowUs;
    DEBUG((DEBUG_INIT, "UCSICr: PMIC_RTR_ADSP_APPS %a\n", mPeerReady ? "open" : "closed"));
  }
  /* Publish PD invalidation before sending its indication ACK. The USB/DP
     monitors then observe the withdrawn connector state on their next poll. */
  Status = CrQrtrTick(&mQrtr, NowUs);
  if(mStopped || mExiting)goto Report;
  if (EFI_ERROR(Status) && Status != EFI_NOT_READY)
    DEBUG((DEBUG_WARN, "UCSICr: QRTR service send failed: %r\n", Status));
  if (mPeerReady) {
    BOOLEAN WasFaulted = mPmic.Faulted;
    Status = CrPmicTick(&mPmic, NowUs);
    if(mStopped || mExiting)goto Report;
    if (WasFaulted && !mPmic.Faulted) mWaitLogged = FALSE;
    if (EFI_ERROR(Status) && !mWaitLogged) {
      UINT32 Available = 0, Busy = 0;
      UINTN Channel, Intent;
      DEBUG((DEBUG_ERROR, "UCSICr: PMIC/UCSI stage=%u command=0x%Lx failed: %r\n",
             (UINT32)mPmic.Stage, mPmic.Command, Status));
      for (Channel = 0; Channel < CR_GLINK_CHANNELS; ++Channel) {
        if (!mLink.Channels[Channel].Pmic) continue;
        for (Intent = 0; Intent < CR_GLINK_REMOTE_INTENTS; ++Intent) {
          CR_GLINK_REMOTE_INTENT *Remote = &mLink.Channels[Channel].Remote[Intent];
          if (!Remote->Present) continue;
          if (Remote->Busy) ++Busy;
          else ++Available;
        }
      }
      DEBUG((DEBUG_ERROR, "UCSICr: GLink queued=%u intents available=%u busy=%u "
             "TX tail/head=%u/%u RX tail/head=%u/%u\n",
              (UINT32)mLink.QueueCount, Available, Busy,
              mLink.Descriptor == NULL ? 0 : mLink.Descriptor[0],
              mLink.Descriptor == NULL ? 0 : mLink.Descriptor[1],
              mLink.Descriptor == NULL ? 0 : mLink.Descriptor[2],
              mLink.Descriptor == NULL ? 0 : mLink.Descriptor[3]));
      DEBUG((DEBUG_ERROR, "UCSICr: QRTR online=%u RX=%u rejected=%u queued=%u "
             "mapper=%u PDR-server=%u listen=%u PD-known=%u PD-ready=%u status=%r\n",
              mQrtr.Online, mQrtr.Received, mQrtr.Malformed, (UINT32)mQrtr.QueueCount,
              mQrtr.DomainRequests, mQrtr.ServerFound, mQrtr.Registered,
              mQrtr.PdKnown, mQrtr.PdReady, mQrtr.LastError));
      mWaitLogged = TRUE;
    }
    if (mPmic.State.Connected && mPmic.State.Orientation == UsbTypeCUnoriented &&
        !mPmic.Faulted && mGpio->ReadIoValue != NULL) {
      UINT8 Value = 0;
      Status = mTarget->LeaseOrientation?
        mGpio->ReadPinValue(mGpio,&mProtocol,mTarget->OrientationGpio,&Value):
        mGpio->ReadIoValue(mGpio,mTarget->OrientationGpio,&Value);
      if (Status==EFI_SUCCESS) {
        mPmic.State.Orientation = (!!Value==mTarget->ReverseHigh) ? UsbTypeCReverse : UsbTypeCNormal;
        ++mPmic.State.Sequence;
      }
    }
  } else if (!mWaitLogged && NowUs - mStartedUs >= 5000000ULL) {
    DEBUG((DEBUG_WARN, "UCSICr: waiting for ADSP PMIC/charger_pd "
           "(RX FIFO=%p version-ack=%u RPMSG=%u QRTR=%u PDR=%u PD-ready=%u)\n",
            mLink.Rx, mLink.VersionAck, mLink.Ready, mQrtr.Online,
            mQrtr.ServerFound, mQrtr.PdReady));
    mWaitLogged = TRUE;
  }
  CrChargerLogTick(&mChargerLog, NowUs, mPeerReady && mPmic.Faulted);
Report:
  if (mPmic.State.Sequence != mLoggedSequence) {
    EFI_USB_TYPEC_STATE *State = &mPmic.State;
    DEBUG((DEBUG_INIT, "UCSICr: ready=%u connected=%u data=%u source=%u orientation=%u "
           "DP=%u lanes=%u pin=%u USB3=%u HPD=%u IRQ=%u status=%r\n",
           State->PdReady, State->Connected, State->DataRole, State->PowerSource,
           State->Orientation, State->DpActive, State->DpLanes, State->PinAssignment,
           State->Usb3, State->Hpd, State->HpdIrqSequence, State->LastError));
    mLoggedSequence = State->Sequence;
  }
  if (!mStopped && !mExiting && !mEntryBusy) {
    BOOLEAN Active = !mPeerReady || mRxPending || CrAtomicLoad32(&mIpcc.Pending) != 0 ||
      mLink.QueueCount != 0 || CrPmicNeedsService(&mPmic) ||
      mQrtr.QueueCount != 0 || mQrtr.Pending ||
      (mQrtr.Online && !mQrtr.HelloSent) ||
      (mChargerLog.Triggered && (!mChargerLog.Done || mChargerLog.Length != 0));
    /* The slow idle watchdog discovers PIL unload/restart only. It neither
       polls GLink RX nor sends GET_CONNECTOR_STATUS while the PPM is idle. */
    Status = gBS->SetTimer(mTimer, TimerRelative, Active ? 200000ULL : 10000000ULL);
    if (EFI_ERROR(Status)) {
      CrPmicFail(&mPmic, Status);
      DEBUG((DEBUG_ERROR, "UCSICr: hot-plug monitor rearm failed: %r\n", Status));
      StopMonitor();
    }
  }
  mServicing = FALSE;
  if(mExiting)(VOID)QuiescePil(NULL);
  return;
TransportFault:
  DEBUG((DEBUG_ERROR, "UCSICr: IRQ/GLink fault %r; transport stopped\n", Status));
  CrPmicFail(&mPmic, Status);
  StopMonitor();
  mServicing = FALSE;
}

STATIC EFI_STATUS StopMonitor(VOID)
{
  EFI_STATUS Status;
  mStopped = TRUE;
  /* Synchronous pure invalidation precedes IRQ drain/PIL PAS shutdown. */
  PublishPmicState(FALSE, EFI_NOT_READY, FALSE);
  if (mTimer != NULL) gBS->SetTimer(mTimer, TimerCancel, 0);
  Status = CrIpccStop(&mIpcc);
  if (EFI_ERROR(Status))
    DEBUG((DEBUG_ERROR, "UCSICr: OSKAL IRQ removal failed %r; retaining stopped callback owner\n", Status));
  return Status;
}

STATIC EFI_STATUS ReleasePdSupply(VOID)
{
  EFI_STATUS Status;
  if (!mPdSupplyOwned) return EFI_SUCCESS;
  if(mTarget==NULL || mTarget->PdRail==NULL)return EFI_COMPROMISED_DATA;
  Status = mRpmh->RpmhEnableVreg(mRpmh,mTarget->PdRail,FALSE);
  if(Status==EFI_SUCCESS)mPdSupplyOwned=FALSE;
  else if(!EFI_ERROR(Status))Status=EFI_NOT_READY;
  return Status;
}
STATIC EFI_STATUS ReleaseOrientation(VOID) {
  if(!mOrientationOwned)return EFI_SUCCESS;
  if(mTarget==NULL||!mTarget->LeaseOrientation)return EFI_COMPROMISED_DATA;
  EFI_STATUS S=mGpio->ReleasePin(mGpio,&mProtocol,mTarget->OrientationGpio);
  if(S==EFI_SUCCESS)mOrientationOwned=FALSE;else if(!EFI_ERROR(S))S=EFI_NOT_READY;
  return S;
}

STATIC CR_STATUS QuiescePil(VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Context;
  /* TPL_NOTIFY excludes new work, but must not certify an interrupted
     TPL_CALLBACK service invocation as drained. A failed quiesce retains
     the PIL client so shutdown cannot race its remaining remote writes. */
  mStopped=TRUE;
  PublishPmicState(FALSE,EFI_NOT_READY,FALSE);
  if (mServicing || mEntryBusy) return EFI_NOT_READY;
  Status = StopMonitor();
  if (EFI_ERROR(Status)) return Status;
  if(mOwnershipUncertain)return EFI_ACCESS_DENIED;
  Status = ReleasePdSupply();
  if (EFI_ERROR(Status))
    DEBUG((DEBUG_ERROR, "UCSICr: PD supply withdrawal failed: %r; retaining ownership\n", Status));
  if(Status==EFI_SUCCESS)Status=ReleaseOrientation();
  return Status;
}

STATIC VOID EFIAPI Exit(EFI_EVENT Event, VOID *Context)
{
  (VOID)Event;
  mExiting=TRUE;
  (VOID)QuiescePil(Context);
}

STATIC EFI_STATUS Cleanup(VOID)
{
  EFI_STATUS Status;
  Status = QuiescePil(NULL);
  if (EFI_ERROR(Status)) return Status;
  if (mPilOwner != NULL) {
    EFI_PIL_CR_PROTOCOL *Owner = NULL;
    Status = gBS->LocateProtocol(&gEfiPilCrProtocolGuid, NULL, (VOID **)&Owner);
    if (EFI_ERROR(Status)) return Status;
    if (Owner != mPilOwner) return EFI_ACCESS_DENIED;
    Status = Owner->UnregisterClient(Owner, &mProtocol);
    if (EFI_ERROR(Status)) return Status;
    mPilOwner = NULL;
  }
  if (mExit != NULL) {Status=gBS->CloseEvent(mExit);if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_NOT_READY;mExit=NULL;}
  if (mTimer != NULL) {Status=gBS->CloseEvent(mTimer);if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_NOT_READY;mTimer=NULL;}
  if (mWork != NULL) {Status=gBS->CloseEvent(mWork);if(Status!=EFI_SUCCESS)return EFI_ERROR(Status)?Status:EFI_NOT_READY;mWork=NULL;}
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI UCSICrUnload(EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  if(mExiting || mEntryBusy || mOwnershipUncertain)return EFI_ACCESS_DENIED;
  EFI_TPL Tpl=gBS->RaiseTPL(TPL_NOTIFY);
  UINTN I;
  for(I=0;I<PMIC_SUBSCRIBERS;++I) if(mSubscribers[I].Identity!=NULL) {
    gBS->RestoreTPL(Tpl); return EFI_ACCESS_DENIED;
  }
  mStopped=TRUE;
  PublishPmicState(FALSE,EFI_NOT_READY,FALSE);
  gBS->RestoreTPL(Tpl);
  Status = StopMonitor();
  if (EFI_ERROR(Status)) return Status;
  if (mPublished) {
    Status = gBS->UninstallMultipleProtocolInterfaces(ImageHandle,
               &gEfiUsbTypeCCrProtocolGuid, &mProtocol,
               &gEfiPmicGlinkCrProtocolGuid, &mPmicProtocol, NULL);
    if (EFI_ERROR(Status)) return Status;
    mPublished = FALSE;
  }
  return Cleanup();
}

STATIC EFI_STATUS StartupError(CONST CHAR8 *Stage, EFI_STATUS Status)
{
  DEBUG((DEBUG_ERROR, "UCSICr: startup %a failed: %r; hot-plug monitor unavailable\n",
         Stage, Status));
  return Status;
}
STATIC BOOLEAN IpccAdmit(VOID *Context){(VOID)Context;return !mExiting;}

STATIC EFI_STATUS StartMonitor(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  GpioConfigParams Config = {0};
  CR_INTERRUPT_CONFIG InterruptConfig = {0};
  (VOID)SystemTable;
  if (mExiting || mOwnershipUncertain || mPublished || mTimer != NULL || mExit != NULL || mWork != NULL || mPdSupplyOwned || mOrientationOwned || mIpcc.Registered || mPilOwner != NULL)
    return EFI_ALREADY_STARTED;
  mTarget=CrUcsiGetTarget();
  if(mTarget==NULL || !((mTarget->Soc==0x8350&&mTarget->Board==8350&&mTarget->IpccBase==0x408000&&mTarget->OrientationGpio==81&&mTarget->LeaseOrientation&&mTarget->PdRail==NULL)||
      (mTarget->Soc==0x8450&&mTarget->Board==8450&&mTarget->IpccBase==0xed18000&&mTarget->OrientationGpio==91&&!mTarget->LeaseOrientation&&mTarget->PdRail!=NULL)) ||
      mTarget->IpccSource!=261)return StartupError("board transport target",EFI_UNSUPPORTED);
  mStopped = TRUE;
  mPeerReady = mWaitLogged = FALSE;
  mPilSeenReady = mRebind = FALSE;
  mServicing = mIrqLogged = FALSE;
  mRxPending = TRUE; /* initial FIFO snapshot, then IRQ-driven RX */
  Status = gBS->LocateProtocol(&gEfiPilCrProtocolGuid, NULL, (VOID **)&mPilLoader);
  if (EFI_ERROR(Status)) return StartupError("PIL provider", Status);
  if (mPilLoader == NULL || mPilLoader->Revision < EFI_PIL_CR_PROTOCOL_REVISION ||
      mPilLoader->GetState == NULL || mPilLoader->RegisterClient == NULL ||
      mPilLoader->UnregisterClient == NULL)
    return StartupError("PIL interface", EFI_INCOMPATIBLE_VERSION);
  mPeerGeneration = mLoggedSequence = mPdGeneration = 0;
  mAdspGeneration=mTransportGeneration=0; mDemuxDispatch=FALSE;
  ZeroMem(mSubscribers,sizeof(mSubscribers)); ZeroMem(&mPmicTransport,sizeof(mPmicTransport));
  mPmicTransport.LastError=EFI_NOT_READY;
  CrQrtrInitialize(&mQrtr, SendQrtr, NULL);
  CrChargerLogInitialize(&mChargerLog, SendLogs, NULL);
  CrGlinkReset(&mLink);
  Status = gBS->LocateProtocol(&gEfiSmemCrProtocolGuid, NULL, (VOID **)&mSmem);
  if (EFI_ERROR(Status)) return StartupError("SMEM protocol", Status);
  Status = gBS->LocateProtocol(&gEfiGpioCrProtocolGuid, NULL, (VOID **)&mGpio);
  if (EFI_ERROR(Status)) return StartupError("GPIO protocol", Status);
  if (mSmem == NULL || mSmem->Revision < EFI_SMEM_CR_PROTOCOL_REVISION ||
      mSmem->GetItem == NULL || mSmem->AllocateItem == NULL ||
      mGpio == NULL || mGpio->ConfigGpio == NULL)
    return StartupError("provider interface", EFI_UNSUPPORTED);
  if(mTarget->LeaseOrientation && (mGpio->Revision<0x10002||mGpio->AcquirePin==NULL||mGpio->ReadPinValue==NULL||mGpio->ReleasePin==NULL))
    return StartupError("orientation lease provider",EFI_UNSUPPORTED);
  Status = gBS->LocateProtocol(&gEfiRpmhCrProtocolGuid, NULL, (VOID **)&mRpmh);
  if (EFI_ERROR(Status)) return StartupError("RPMh protocol", Status);
  if (mRpmh == NULL || mRpmh->Revision < EFI_RPMH_CR_PROTOCOL_REVISION ||
      mRpmh->AcquireVreg == NULL || mRpmh->RpmhEnableVreg == NULL)
    return StartupError("RPMh interface", EFI_UNSUPPORTED);
  Config.PinNumber = mTarget->OrientationGpio;
  Config.FunctionSel = GPIO_FUNC_NORMAL;
  Config.Pull = GPIO_PULL_NONE;
  Config.DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE;
  Config.OutputValue = GPIO_VALUE_UNCHANGE;
  if(mTarget->LeaseOrientation){
    /* Public lahaina usb3phy_portselect_gpio: input, normal mux, pull-up,
       2 mA. The lease provider preserves the latent output latch under its
       snapshot lock rather than guessing a value for this input. */
    Config.Pull=GPIO_PULL_UP;
    Config.DriveStrength=GPIO_DRIVE_STRENGTH_2MA;
  }
  if(!mTarget->LeaseOrientation){
    Status=mGpio->ConfigGpio(mGpio,&Config);
    if(Status!=EFI_SUCCESS)return StartupError("orientation GPIO",EFI_ERROR(Status)?Status:EFI_NOT_READY);
  }
  mLink.Receive = Receive; mLink.Kick = Kick;
  mLink.QrtrReceive = ReceiveQrtr; mLink.QrtrState = QrtrState;
  mLink.LogsReceive = ReceiveLogs; mLink.LogsState = LogsState;
  CrPmicInitialize(&mPmic, Send, NULL);
  mSmemOps.Get = SmemGet; mSmemOps.Allocate = SmemAllocate; mSmemOps.Context = mSmem;
  Status = CrGlinkBind(&mLink, &mSmemOps);
  if (EFI_ERROR(Status)) return StartupError("ADSP GLink bind", Status);
  Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Service, NULL, &mTimer);
  if (EFI_ERROR(Status)) return StartupError("poll event", Status);
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_NOTIFY, Exit, NULL,
                             &gEfiEventExitBootServicesGuid, &mExit);
  if (EFI_ERROR(Status)) {
    StartupError("exit event", Status);goto Rollback;
  }
  if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  Status = gBS->CreateEvent(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Service, NULL, &mWork);
  if (EFI_ERROR(Status)) { StartupError("IRQ work event", Status); goto Rollback; }
  if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  /* Pin PIL before any IRQ or deferred remote writer can be started, even if
     later startup fails before publishing the Type-C protocol. PIL calls this
     synchronous barrier itself; exit-event registration order is irrelevant. */
  Status = mPilLoader->RegisterClient(mPilLoader, &mProtocol, QuiescePil, NULL);
  if (EFI_ERROR(Status)) { StartupError("PIL consumer registration", Status); goto Rollback; }
  mPilOwner = mPilLoader;
  if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  if(mTarget->LeaseOrientation){
    Status=mGpio->AcquirePin(mGpio,&mProtocol,&Config,&mOrientationOwned);
    if(Status!=EFI_SUCCESS||!mOrientationOwned){
      if(Status==EFI_SUCCESS){mOwnershipUncertain=TRUE;Status=EFI_NOT_READY;}
      else if(!EFI_ERROR(Status)){if(!mOrientationOwned)mOwnershipUncertain=TRUE;Status=EFI_NOT_READY;}
      StartupError("orientation GPIO lease",Status);goto Rollback;
    }
    if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  }
  /* HDK schematic sheets 23/33: L2B feeds PM8350BH VDD_PDPHY/VDD_VCONN
     as well as the AP HS PHY. Power it before any UCSI command, independent
     of cable state; USB PHY ownership starts only after UCSI role detection.
     RpmhCrDxe acquires its BOB input first and shares the L2B vote with USB. */
  if(mTarget->PdRail!=NULL){
    Status=mRpmh->AcquireVreg(mRpmh,mTarget->PdRail,mTarget->PdMillivolts,mTarget->PdMode,&mPdSupplyOwned);
    if(Status!=EFI_SUCCESS||!mPdSupplyOwned){
      if(Status==EFI_SUCCESS){mOwnershipUncertain=TRUE;Status=EFI_NOT_READY;}else if(!EFI_ERROR(Status)){if(!mPdSupplyOwned)mOwnershipUncertain=TRUE;Status=EFI_NOT_READY;}
      StartupError("PD supply",Status);goto Rollback;
    }
    if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
    DEBUG((DEBUG_INIT,"UCSICr: board PD supply %a %umV acquired before PMIC negotiation\n",mTarget->PdRail,mTarget->PdMillivolts));
  }
  InterruptConfig.InterruptNumber = mTarget->IpccSource;
  InterruptConfig.TriggerType = CR_INTERRUPT_TRIGGER_LEVEL_HIGH;
  Status = CrIpccStartGuarded(&mIpcc,mTarget->IpccBase,&InterruptConfig,Wake,NULL,IpccAdmit,NULL);
  if (EFI_ERROR(Status)) { StartupError("OSKAL IPCC IRQ", Status); goto Rollback; }
  if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  DEBUG((DEBUG_WARN,"UCSICr: IPCC base=%lx IRQ=%u LPASS=3 signal=0 GPIO%u armed\n",mTarget->IpccBase,mTarget->IpccSource,mTarget->OrientationGpio));
  mProtocol.Revision = EFI_USB_TYPEC_CR_PROTOCOL_REVISION;
  mProtocol.GetState = GetState;
  mProtocol.SetDataRole = SetDataRole;
  mProtocol.SetPowerRole = SetPowerRole;
  mProtocol.GetNonPdHostWitness=NULL; /* GLINK uses actual negotiated UCSI. */
  mPmicProtocol.Revision=EFI_PMIC_GLINK_CR_PROTOCOL_REVISION;
  mPmicProtocol.Register=PmicRegister; mPmicProtocol.Unregister=PmicUnregister;
  mPmicProtocol.Send=PmicSend; mPmicProtocol.GetState=PmicGetState;
  Status = gBS->InstallMultipleProtocolInterfaces(&ImageHandle, &gEfiUsbTypeCCrProtocolGuid,
                                                 &mProtocol,
                                                 &gEfiPmicGlinkCrProtocolGuid, &mPmicProtocol, NULL);
  if (EFI_ERROR(Status)) {
    StartupError("Type-C protocol publication", Status);
    goto Rollback;
  }
  mPublished = TRUE;
  if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  mStartedUs = GetTimeInNanoSecond(GetPerformanceCounter()) / 1000ULL;
  mStopped = FALSE;
  Status = gBS->SetTimer(mTimer, TimerRelative, 200000ULL);
  if(mExiting){Status=EFI_NOT_READY;goto Rollback;}
  if (EFI_ERROR(Status)) {
    mStopped = TRUE;
    CrPmicFail(&mPmic, Status);
    DEBUG((DEBUG_ERROR, "UCSICr: hot-plug monitor timer failed: %r\n", Status));
    if (EFI_ERROR(gBS->UninstallMultipleProtocolInterfaces(ImageHandle,
        &gEfiUsbTypeCCrProtocolGuid, &mProtocol,
        &gEfiPmicGlinkCrProtocolGuid, &mPmicProtocol, NULL))) return EFI_SUCCESS;
    mPublished = FALSE;
    goto Rollback;
  }
  DEBUG((DEBUG_INIT, "UCSICr: native ADSP GLink/UCSI/PAN monitor armed through Shell\n"));
  return EFI_SUCCESS;
Rollback:
  mEntryBusy=FALSE;
  if(mExiting){(VOID)QuiescePil(NULL);return EFI_SUCCESS;}
  /* A failed vote may have reached the PMIC. Keep this image and its exit
     event resident if withdrawal fails, so ExitBootServices/unload can retry. */
  {
    EFI_STATUS CleanupStatus = Cleanup();
    if (EFI_ERROR(CleanupStatus)) {
      DEBUG((DEBUG_ERROR, "UCSICr: startup cleanup failed: %r; stopped image retained for retry\n",
             CleanupStatus));
      return EFI_SUCCESS;
    }
  }
  return Status;
}

EFI_STATUS EFIAPI UCSICrEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  if(mEntryBusy || mExiting || mOwnershipUncertain)return EFI_ALREADY_STARTED;
  mEntryBusy=TRUE;
  Status=StartMonitor(ImageHandle,SystemTable);
  mEntryBusy=FALSE;
  return Status;
}
