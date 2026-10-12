/** USB2 control/bulk UsbFunctionIo, using Linux DWC3 gadget/EP0 registers.
    Copyright (c) 2010-2020, The Linux Foundation and Synopsys.
    SPDX-License-Identifier: GPL-2.0-only */
#include "Dwc3.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

typedef struct { UINT32 Low, High, Size, Control; } USB_TRB;
typedef struct {
  CR_USB_DMA Data;
  VOID *Buffer;
  UINTN Requested, Programmed;
  UINT8 Resource;
  UINT64 EndStarted;
  BOOLEAN Enabled, Active, Pending, Internal, Stalled, Ending, EndDone, Zlp, Chained;
} USB_ENDPOINT;
typedef struct { EFI_USBFN_MESSAGE Message; UINTN Size; EFI_USBFN_MESSAGE_PAYLOAD Payload; } USB_MESSAGE;
typedef struct { VOID *Cpu; UINTN Pages; } USB_CLIENT_BUFFER;
STATIC EDKII_IOMMU_PROTOCOL *mIommu;
STATIC CR_USB_DMA mEvents, mDescriptors;
STATIC USB_ENDPOINT mEndpoints[USB_MAX_EP];
STATIC USB_CLIENT_BUFFER mBuffers[32];
STATIC USB_MESSAGE mMessages[64];
STATIC UINTN mMessageHead, mMessageCount, mEventOffset;
STATIC BOOLEAN mFrozen;
STATIC BOOLEAN mAvailable, mStarted, mConfigured, mDmaArmed, mStatusReady, mControlAuto, mRestartControl;
STATIC BOOLEAN mReconfigureBulk, mBulkConfigured, mSetupPending;
STATIC EFI_USB_DEVICE_REQUEST mConfigRequest;
STATIC EFI_STATUS mFault;
STATIC EFI_USB_BUS_SPEED mSpeed;
STATIC UINT8 mPhase, mDataDirection, mStatusDirection, mAddress, mConfigValue;
STATIC UINT16 mControlLength, mVid, mPid;
STATIC EFI_LOCK mLock = EFI_INITIALIZE_LOCK_VARIABLE(TPL_NOTIFY);

STATIC USB_TRB *Trbs(UINT8 Ep)
{ return (USB_TRB *)((UINT8 *)mDescriptors.Cpu + 64 + Ep * 64); }

EFI_STATUS CrUsbDmaFree(CR_USB_DMA *Dma)
{
  EFI_STATUS Status;
  if (mFrozen) return EFI_NOT_READY;
  if (Dma->Mapping != NULL) {
    Status = mIommu->Unmap(mIommu, Dma->Mapping);
    if (EFI_ERROR(Status)) return Status;
    Dma->Mapping = NULL;
  }
  if (Dma->Cpu != NULL) {
    Status = mIommu->FreeBuffer(mIommu, Dma->Pages, Dma->Cpu);
    if (EFI_ERROR(Status)) return Status;
    ZeroMem(Dma, sizeof(*Dma));
  }
  return EFI_SUCCESS;
}
EFI_STATUS CrUsbDmaAllocate(CR_USB_DMA *Dma, UINTN Size)
{
  UINTN Bytes;
  EFI_STATUS Status;
  if (mFrozen) return EFI_NOT_READY;
  if (Dma->Cpu != NULL || Dma->Mapping != NULL || Size == 0 || Size > USB_MAX_TRANSFER + 512)
    return EFI_INVALID_PARAMETER;
  Dma->Pages = EFI_SIZE_TO_PAGES(Size);
  Status = mIommu->AllocateBuffer(mIommu, AllocateAnyPages, EfiBootServicesData,
                                 Dma->Pages, &Dma->Cpu, 0);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem(Dma->Cpu, EFI_PAGES_TO_SIZE(Dma->Pages));
  WriteBackInvalidateDataCacheRange(Dma->Cpu, EFI_PAGES_TO_SIZE(Dma->Pages));
  Bytes = EFI_PAGES_TO_SIZE(Dma->Pages);
  Status = mIommu->Map(mIommu, EdkiiIoMmuOperationBusMasterCommonBuffer64,
                        Dma->Cpu, &Bytes, &Dma->Device, &Dma->Mapping);
  if (EFI_ERROR(Status)) return Status;
  if (Bytes != EFI_PAGES_TO_SIZE(Dma->Pages)) return EFI_BAD_BUFFER_SIZE;
  return mIommu->SetAttributeById(mIommu, 0x15000000, 0, Dma->Mapping,
                                 EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE);
}
STATIC VOID Queue(EFI_USBFN_MESSAGE Message, CONST VOID *Payload, UINTN Size)
{
  USB_MESSAGE *Entry;
  if (mMessageCount == ARRAY_SIZE(mMessages)) { mFault = EFI_OUT_OF_RESOURCES; return; }
  Entry = &mMessages[(mMessageHead + mMessageCount++) % ARRAY_SIZE(mMessages)];
  ZeroMem(Entry, sizeof(*Entry));
  Entry->Message = Message; Entry->Size = Size;
  if (Size != 0) CopyMem(&Entry->Payload, Payload, Size);
}
STATIC EFI_STATUS Command(UINT8 Ep, UINT32 Cmd, UINT32 P0, UINT32 P1, UINT32 P2)
{
  EFI_STATUS Status;
  MmioWrite32(USB_EP(Ep) + 8, P0); MmioWrite32(USB_EP(Ep) + 4, P1); MmioWrite32(USB_EP(Ep), P2);
  MmioWrite32(USB_EP(Ep) + 12, Cmd | BIT10);
  Status = CrUsbWait(USB_EP(Ep) + 12, BIT10, 0, 10000);
  if (!EFI_ERROR(Status) && (MmioRead32(USB_EP(Ep) + 12) & 0xF000) != 0) Status = EFI_DEVICE_ERROR;
  if (EFI_ERROR(Status)) DEBUG((DEBUG_ERROR, "Dwc3Cr: endpoint=%u command=%x result=%08x: %r\n",
                                Ep, Cmd, MmioRead32(USB_EP(Ep) + 12), Status));
  return Status;
}
STATIC EFI_STATUS ConfigureEndpoint(UINT8 Ep, BOOLEAN Modify)
{
  UINT32 P0 = ((Ep < 2 ? 64 : mSpeed == UsbBusSpeedFull ? 64 : 512) << 3) |
              ((Ep < 2 ? 0U : 2U) << 1) | ((Ep & 1) ? ((UINT32)Ep / 2) << 17 : 0);
  EFI_STATUS Status;
  if (Modify) P0 |= 2U << 30;
  Status = Command(Ep, 1, P0, ((UINT32)Ep << 25) | BIT8 | BIT10, 0);
  if (!EFI_ERROR(Status) && !Modify) Status = Command(Ep, 2, 1, 0, 0);
  return Status;
}
STATIC EFI_STATUS Submit(UINT8 Ep, UINTN Bytes, UINT8 Type, BOOLEAN Zlp)
{
  USB_TRB *T = Trbs(Ep);
  USB_ENDPOINT *E = &mEndpoints[Ep];
  EFI_PHYSICAL_ADDRESS Data = Type == 2 ? mDescriptors.Device : E->Data.Device;
  EFI_PHYSICAL_ADDRESS Address = mDescriptors.Device + 64 + Ep * 64;
  EFI_STATUS Status;
  ZeroMem(T, sizeof(USB_TRB) * 2);
  T[0].Low = (UINT32)Data; T[0].High = (UINT32)(Data >> 32); T[0].Size = (UINT32)Bytes;
  T[0].Control = (UINT32)Type << 4 | BIT0 | (Zlp ? BIT2 : BIT1 | BIT11) | BIT10;
  if (Zlp) {
    T[1].Low = (UINT32)Data; T[1].High = (UINT32)(Data >> 32);
    T[1].Control = (UINT32)Type << 4 | BIT0 | BIT1 | BIT11 | BIT10;
  }
  E->Programmed = Bytes;
  E->Chained = Zlp;
  if (Type != 2 && Bytes != 0) WriteBackInvalidateDataCacheRange(E->Data.Cpu, Bytes);
  WriteBackDataCacheRange(T, sizeof(USB_TRB) * 2);
  MemoryFence();
  /* A failed/uncertain start still owns its descriptor until controller halt. */
  E->Active = TRUE;
  Status = Command(Ep, 6, (UINT32)(Address >> 32), (UINT32)Address, 0);
  if (EFI_ERROR(Status)) { mFault = Status; return Status; }
  E->Resource = (UINT8)((MmioRead32(USB_EP(Ep) + 12) >> 16) & 0x7F);
  if (E->Resource == 0 && Ep >= 2) { mFault = EFI_PROTOCOL_ERROR; return mFault; }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Setup(VOID)
{
  mPhase = 0; mStatusReady = mControlAuto = mSetupPending = FALSE;
  mEndpoints[0].Internal = TRUE;
  mEndpoints[0].Requested = 8;
  InvalidateDataCacheRange(mDescriptors.Cpu, 64);
  return Submit(0, 8, 2, FALSE);
}
STATIC VOID Complete(UINT8 Ep, EFI_USBFN_TRANSFER_STATUS State)
{
  USB_ENDPOINT *E = &mEndpoints[Ep];
  USB_TRB *T = Trbs(Ep);
  UINTN Actual = 0;
  EFI_USBFN_TRANSFER_RESULT Result;
  if (!E->Active && !E->Pending) return;
  if (State == UsbTransferStatusComplete) {
    InvalidateDataCacheRange(T, sizeof(USB_TRB) * 2);
    if (Ep < 2 && mPhase == 1 && (T[0].Size >> 28) == 2) mSetupPending = TRUE;
    if ((T[0].Size & 0xFFFFFF) <= E->Programmed)
      Actual = E->Programmed - (T[0].Size & 0xFFFFFF);
    if ((T[0].Control & BIT0) != 0 || Actual > E->Requested ||
        (T[0].Size >> 28) != 0 || (E->Chained &&
        ((T[1].Control & BIT0) != 0 || T[1].Size != 0))) {
      State = UsbTransferStatusAborted; Actual = 0;
    } else if (!(Ep & 1) && Actual != 0 && !E->Internal) {
      InvalidateDataCacheRange(E->Data.Cpu, E->Programmed);
      CopyMem(E->Buffer, E->Data.Cpu, Actual);
    }
  }
  if (!E->Internal) {
    Result = (EFI_USBFN_TRANSFER_RESULT){Actual, State, (UINT8)(Ep / 2),
               (Ep & 1) ? EfiUsbEndpointDirectionDeviceTx : EfiUsbEndpointDirectionDeviceRx, E->Buffer};
    Queue((Ep & 1) ? EfiUsbMsgEndpointStatusChangedTx : EfiUsbMsgEndpointStatusChangedRx,
           &Result, sizeof(Result));
  }
  E->Active = E->Pending = E->Internal = FALSE; E->Resource = 0; E->Buffer = NULL;
}
STATIC EFI_STATUS StallControl(VOID)
{
  EFI_STATUS Status;
  Complete(0, UsbTransferStatusAborted); Complete(1, UsbTransferStatusAborted);
  Status = Command(0, 4, 0, 0, 0);
  if (!EFI_ERROR(Status)) Status = Setup();
  if (EFI_ERROR(Status)) mFault = Status;
  return Status;
}
STATIC EFI_STATUS End(UINT8 Ep)
{
  USB_ENDPOINT *E = &mEndpoints[Ep];
  EFI_STATUS Status;
  if (E->Ending) return EFI_SUCCESS;
  if (!E->Active) { Complete(Ep, UsbTransferStatusAborted); return EFI_SUCCESS; }
  E->EndDone = FALSE;
  E->Ending = TRUE;
  E->EndStarted = GetTimeInNanoSecond(GetPerformanceCounter());
  Status = Command(Ep, 8 | BIT8 | BIT11 | ((UINT32)E->Resource << 16), 0, 0, 0);
  if (EFI_ERROR(Status)) mFault = Status;
  return Status;
}
STATIC EFI_STATUS StartStatus(VOID)
{
  UINT8 Ep = mStatusDirection;
  USB_ENDPOINT *E = &mEndpoints[Ep];
  if (!mStatusReady || !E->Pending || E->Active || E->Ending) return EFI_SUCCESS;
  return Submit(Ep, 0, mControlLength == 0 ? 3 : 4, FALSE);
}
STATIC EFI_STATUS ConfigureBulk(VOID)
{
  EFI_STATUS Status;
  UINTN I;
  UINT32 Enabled = 3;
  Status = Command(0, 9 | (2U << 16), 0, 0, 0);
  if (EFI_ERROR(Status)) return Status;
  mBulkConfigured = mConfigRequest.Value != 0;
  for (I = 2; I < USB_MAX_EP; ++I) if (mEndpoints[I].Enabled && mBulkConfigured) {
    Status = ConfigureEndpoint((UINT8)I, FALSE);
    if (EFI_ERROR(Status)) return Status;
    mEndpoints[I].Stalled = FALSE;
    Enabled |= (UINT32)1 << I;
  }
  MmioWrite32(USB_DALEPENA, Enabled);
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PollLocked(VOID)
{
  UINT32 Count, Event, Kind, Ep;
  EFI_STATUS Status;
  UINTN I;
  if (mFrozen) return EFI_NOT_READY;
  if (!mStarted || !mConfigured) return EFI_ERROR(mFault) ? mFault : EFI_SUCCESS;
  if (EFI_ERROR(mFault)) return mFault;
  Count = MmioRead32(USB_REG(0xC40C)) & 0xFFFF;
  if (Count > USB_EVENT_SIZE || (Count & 3) != 0) { mFault = EFI_PROTOCOL_ERROR; return mFault; }
  while (Count != 0 && mMessageCount < ARRAY_SIZE(mMessages) - USB_MAX_EP - 4) {
    InvalidateDataCacheRange((UINT8 *)mEvents.Cpu + mEventOffset, sizeof(Event));
    CopyMem(&Event, (UINT8 *)mEvents.Cpu + mEventOffset, sizeof(Event));
    mEventOffset = (mEventOffset + 4) % USB_EVENT_SIZE;
    MmioWrite32(USB_REG(0xC40C), 4); Count -= 4;
    if ((Event & BIT0) != 0) {
      if ((Event & 0xFE) != 0) continue;
      Kind = (Event >> 8) & 15;
      if (Kind == 0 || Kind == 1) {
        /* Keep Run/Stop asserted across USB reset. Retire resources through
           ENDTRANSFER completion; an already armed setup TRB remains valid. */
        mRestartControl = mPhase != 0 || !mEndpoints[0].Active;
        mSetupPending = FALSE;
        for (I = 0; I < USB_MAX_EP; ++I) {
          if (I == 0 && mPhase == 0 && mEndpoints[0].Active) continue;
          Status = End((UINT8)I);
          if (EFI_ERROR(Status)) return Status;
          if (mEndpoints[I].Stalled) {
            Status = Command((UINT8)I, 5, 0, 0, 0);
            if (EFI_ERROR(Status)) { mFault = Status; return Status; }
            mEndpoints[I].Stalled = FALSE;
          }
        }
        MmioAnd32(USB_DCFG, (UINT32)~(0x7FU << 3)); mAddress = 0;
        MmioAnd32(USB_DCTL, (UINT32)~(15U << 1));
        mSpeed = UsbBusSpeedUnknown;
        mReconfigureBulk = mBulkConfigured = FALSE;
        Queue(Kind == 0 ? EfiUsbMsgBusEventDetach : EfiUsbMsgBusEventReset, NULL, 0);
      } else if (Kind == 2) {
        UINT32 Speed = MmioRead32(USB_DSTS) & 7;
        if (Speed > 1) { mFault = EFI_UNSUPPORTED; return mFault; }
        mSpeed = Speed == 0 ? UsbBusSpeedHigh : UsbBusSpeedFull;
        for (I = 0; I < USB_MAX_EP; ++I) if (mEndpoints[I].Enabled && (I < 2 || mBulkConfigured)) {
          Status = ConfigureEndpoint((UINT8)I, TRUE);
          if (EFI_ERROR(Status)) { mFault = Status; return Status; }
        }
        Queue(EfiUsbMsgBusEventSpeed, &mSpeed, sizeof(mSpeed));
      } else if (Kind == 4) Queue(EfiUsbMsgBusEventResume, NULL, 0);
      else if (Kind == 6) Queue(EfiUsbMsgBusEventSuspend, NULL, 0);
      else if (Kind == 9 || Kind == 11) { mFault = EFI_DEVICE_ERROR; return mFault; }
      continue;
    }
    Ep = (Event >> 1) & 31; Kind = (Event >> 6) & 15;
    if (Kind == 7 && ((Event >> 24) & 15) == 8 && mEndpoints[Ep].Ending) {
      if ((Event & 0xF000) != 0) { mFault = EFI_DEVICE_ERROR; return mFault; }
      mEndpoints[Ep].Ending = FALSE; mEndpoints[Ep].EndDone = TRUE;
      Complete((UINT8)Ep, UsbTransferStatusAborted);
      continue;
    }
    if (Kind == 3 && Ep < 2 && ((Event >> 12) & 3) == 2) {
      if (mPhase == 2 && Ep == mStatusDirection) {
        /* A new SETUP aborted DATA. Recover at status NRDY even when the
           function client has not submitted a status transfer. */
        if (mSetupPending) Status = StallControl();
        else { mStatusReady = TRUE; Status = StartStatus(); }
        if (EFI_ERROR(Status)) return Status;
      }
      continue;
    }
    if (Kind != 1 || !mEndpoints[Ep].Active || mEndpoints[Ep].Ending) continue;
    if (Ep == 0 && mPhase == 0) {
      EFI_USB_DEVICE_REQUEST Request;
      InvalidateDataCacheRange(mDescriptors.Cpu, 64);
      InvalidateDataCacheRange(Trbs(0), sizeof(USB_TRB));
      if ((Trbs(0)->Control & BIT0) != 0 || (Trbs(0)->Size & 0xFFFFFF) != 0) {
        mFault = EFI_PROTOCOL_ERROR; return mFault;
      }
      CopyMem(&Request, mDescriptors.Cpu, sizeof(Request));
      mEndpoints[0].Active = mEndpoints[0].Internal = FALSE;
      mControlLength = Request.Length; mDataDirection = (Request.RequestType & BIT7) ? 1 : 0;
      mStatusDirection = mControlLength == 0 ? 1 : mDataDirection ^ 1;
      mPhase = mControlLength == 0 ? 2 : 1; mStatusReady = FALSE;
      /* UsbFunctionIo has no SetAddress method. DWC3 requires DCFG to be
         programmed before starting the SET_ADDRESS status transfer. */
      mControlAuto = Request.RequestType == 0 && Request.Request == 5 &&
                     Request.Index == 0 && Request.Length == 0 && Request.Value <= 127;
      if (mControlAuto) {
        mAddress = (UINT8)Request.Value;
        MmioAndThenOr32(USB_DCFG, (UINT32)~(0x7FU << 3), (UINT32)mAddress << 3);
        mEndpoints[1].Pending = mEndpoints[1].Internal = TRUE;
        mEndpoints[1].Requested = 0;
      } else if (Request.RequestType == 0 && Request.Request == 9 && Request.Index == 0 &&
                 Request.Length == 0 && (Request.Value == 0 || Request.Value == mConfigValue)) {
        mConfigRequest = Request; mReconfigureBulk = TRUE; mBulkConfigured = FALSE;
        for (I = 2; I < USB_MAX_EP; ++I) {
          Status = End((UINT8)I);
          if (EFI_ERROR(Status)) return Status;
        }
      } else Queue(EfiUsbMsgSetupPacket, &Request, sizeof(Request));
    } else {
      Complete((UINT8)Ep, UsbTransferStatusComplete);
      if (Ep < 2) {
        if (mPhase == 1) { mPhase = 2; mStatusReady = FALSE; }
        else if (mPhase == 2) {
          Status = Setup();
          if (EFI_ERROR(Status)) return Status;
        }
      }
    }
  }
  for (I = 0; I < USB_MAX_EP; ++I) if (mEndpoints[I].Ending &&
      GetTimeInNanoSecond(GetPerformanceCounter()) - mEndpoints[I].EndStarted > 100000000) {
    mFault = EFI_TIMEOUT; return mFault;
  }
  if (mRestartControl && !mEndpoints[0].Active && !mEndpoints[1].Active &&
      !mEndpoints[0].Ending && !mEndpoints[1].Ending) {
    mRestartControl = FALSE;
    Status = Setup();
    if (EFI_ERROR(Status)) return Status;
  }
  if (mReconfigureBulk) {
    for (I = 2; I < USB_MAX_EP; ++I) if (mEndpoints[I].Ending) break;
    if (I == USB_MAX_EP) {
      Status = ConfigureBulk();
      if (EFI_ERROR(Status)) { mFault = Status; return Status; }
      mReconfigureBulk = FALSE;
      Queue(EfiUsbMsgSetupPacket, &mConfigRequest, sizeof(mConfigRequest));
    }
  }
  return EFI_SUCCESS;
}
EFI_STATUS CrUsbFnPoll(VOID)
{
  EFI_STATUS Status;
  EfiAcquireLock(&mLock); Status = PollLocked(); EfiReleaseLock(&mLock);
  return Status;
}
VOID CrUsbFnInit(EDKII_IOMMU_PROTOCOL *Iommu) { if (!mFrozen) mIommu = Iommu; }
EFI_STATUS CrUsbFnFreeze(UINT32 *ActiveCalls)
{
  if (ActiveCalls == NULL) return EFI_INVALID_PARAMETER;
  // Every mutator runs to completion under this TPL_NOTIFY lock on the BSP.
  // No client callbacks are made while it is held. Acquiring it drains work.
  EfiAcquireLock(&mLock);
  mFrozen = TRUE;
  mAvailable = FALSE;
  *ActiveCalls = 0;
  EfiReleaseLock(&mLock);
  return EFI_SUCCESS;
}
VOID CrUsbFnSetAvailable(BOOLEAN Available)
{
  EfiAcquireLock(&mLock);
  if (mFrozen) { EfiReleaseLock(&mLock); return; }
  if (mAvailable != Available) Queue(Available ? EfiUsbMsgBusEventAttach : EfiUsbMsgBusEventDetach, NULL, 0);
  mAvailable = Available;
  EfiReleaseLock(&mLock);
}
STATIC EFI_STATUS StopLocked(VOID)
{
  EFI_STATUS Status;
  UINTN I;
  if (mFrozen) return EFI_NOT_READY;
  if (mDmaArmed) {
    Status = CrUsbCoreHalt(UsbTypeCDevice);
    if (EFI_ERROR(Status)) return Status;
    MmioWrite32(USB_DEVTEN, 0); MmioWrite32(USB_DALEPENA, 0);
    MmioWrite32(USB_REG(0xC408), BIT31); MmioWrite32(USB_REG(0xC400), 0); MmioWrite32(USB_REG(0xC404), 0);
    MmioWrite32(USB_REG(0xC40C), MmioRead32(USB_REG(0xC40C)) & 0xFFFC);
    MemoryFence();
    mDmaArmed = FALSE;
  }
  for (I = 0; I < USB_MAX_EP; ++I) {
    if (mDescriptors.Cpu != NULL) Complete((UINT8)I, UsbTransferStatusAborted);
    Status = CrUsbDmaFree(&mEndpoints[I].Data);
    if (EFI_ERROR(Status)) return Status;
    mEndpoints[I].Enabled = mEndpoints[I].Stalled = FALSE;
    mEndpoints[I].Ending = FALSE;
  }
  mConfigured = mStarted = FALSE;
  mRestartControl = mSetupPending = FALSE;
  mReconfigureBulk = mBulkConfigured = FALSE;
  Status = CrUsbDmaFree(&mDescriptors);
  if (EFI_ERROR(Status)) return Status;
  Status = CrUsbDmaFree(&mEvents);
  if (!EFI_ERROR(Status)) mFault = EFI_SUCCESS;
  return Status;
}
EFI_STATUS CrUsbFnStop(VOID)
{
  EFI_STATUS Status;
  EfiAcquireLock(&mLock); Status = StopLocked(); EfiReleaseLock(&mLock);
  return Status;
}
BOOLEAN CrUsbFnCanUnload(VOID)
{
  UINTN I;
  if (mFrozen) return FALSE;
  for (I = 0; I < ARRAY_SIZE(mBuffers); ++I) if (mBuffers[I].Cpu != NULL) return FALSE;
  return mEvents.Cpu == NULL && mDescriptors.Cpu == NULL;
}
STATIC EFI_STATUS EFIAPI StartController(EFI_USBFN_IO_PROTOCOL *This)
{
  EFI_STATUS Status;
  if (This != &gCrUsbFunctionIo) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock);
  if (mFrozen || !mAvailable) { Status = EFI_NOT_READY; goto Out; }
  if (mStarted) { Status = EFI_SUCCESS; goto Out; }
  if (mEvents.Cpu != NULL || mDescriptors.Cpu != NULL) { Status = EFI_NOT_READY; goto Out; }
  mFault = EFI_SUCCESS; mSpeed = UsbBusSpeedUnknown; mEventOffset = 0;
  Status = CrUsbDmaAllocate(&mEvents, USB_EVENT_SIZE);
  if (!EFI_ERROR(Status)) Status = CrUsbDmaAllocate(&mDescriptors, EFI_PAGE_SIZE);
  if (EFI_ERROR(Status)) { mFault = Status; StopLocked(); goto Out; }
  MmioAnd32(USB_DCTL, (UINT32)~(BIT31 | BIT9 | BIT10 | BIT11 | BIT12));
  MmioAndThenOr32(USB_DCFG, (UINT32)~(7U | (0x7FU << 3) | BIT22), 0);
  mDmaArmed = TRUE;
  MmioWrite32(USB_REG(0xC400), (UINT32)mEvents.Device);
  MmioWrite32(USB_REG(0xC404), (UINT32)(mEvents.Device >> 32));
  MmioWrite32(USB_REG(0xC408), USB_EVENT_SIZE | BIT31); /* polled, never unmask CPU IRQ */
  mStarted = TRUE;
Out:
  EfiReleaseLock(&mLock);
  return Status;
}
STATIC EFI_STATUS EFIAPI StopController(EFI_USBFN_IO_PROTOCOL *This)
{ return This == &gCrUsbFunctionIo ? CrUsbFnStop() : EFI_INVALID_PARAMETER; }
STATIC EFI_STATUS EFIAPI Configure(EFI_USBFN_IO_PROTOCOL *This, EFI_USB_DEVICE_INFO *Info)
{
  EFI_USB_INTERFACE_INFO *Interface;
  EFI_USB_CONFIG_INFO *Config;
  EFI_STATUS Status;
  UINT32 Enabled = 3;
  UINTN I, J;
  BOOLEAN Interfaces[256] = {0};
  if (This != &gCrUsbFunctionIo || Info == NULL || Info->DeviceDescriptor == NULL ||
      Info->DeviceDescriptor->NumConfigurations != 1 || Info->ConfigInfoTable == NULL ||
      Info->ConfigInfoTable[0] == NULL) return EFI_INVALID_PARAMETER;
  Config = Info->ConfigInfoTable[0];
  if (Config->ConfigDescriptor == NULL || Config->ConfigDescriptor->NumInterfaces == 0 ||
      Config->ConfigDescriptor->ConfigurationValue == 0 ||
      Config->InterfaceInfoTable == NULL) return EFI_UNSUPPORTED;
  for (I = 0; I < Config->ConfigDescriptor->NumInterfaces; ++I) {
    Interface = Config->InterfaceInfoTable[I];
    if (Interface == NULL || Interface->InterfaceDescriptor == NULL ||
        Interface->InterfaceDescriptor->AlternateSetting != 0 ||
        (Interface->InterfaceDescriptor->NumEndpoints != 0 && Interface->EndpointDescriptorTable == NULL))
      return EFI_INVALID_PARAMETER;
    UINT8 Number = Interface->InterfaceDescriptor->InterfaceNumber;
    if (Interfaces[Number]) return EFI_INVALID_PARAMETER;
    Interfaces[Number] = TRUE;
    for (J = 0; J < Interface->InterfaceDescriptor->NumEndpoints; ++J) {
      EFI_USB_ENDPOINT_DESCRIPTOR *D = Interface->EndpointDescriptorTable[J];
      if (D == NULL || (D->Attributes & 3) != 2 || (D->EndpointAddress & 0x70) != 0 ||
          (D->EndpointAddress & 15) == 0) return EFI_UNSUPPORTED;
      UINT8 Ep = (D->EndpointAddress & 15) * 2 + ((D->EndpointAddress & BIT7) ? 1 : 0);
      if ((Enabled & ((UINT32)1 << Ep)) != 0) return EFI_INVALID_PARAMETER;
      Enabled |= (UINT32)1 << Ep;
    }
  }
  EfiAcquireLock(&mLock);
  if (mFrozen || !mAvailable || !mStarted || mConfigured || EFI_ERROR(mFault)) { Status = EFI_NOT_READY; goto Out; }
  UINT32 Hwp = MmioRead32(USB_REG(0xC14C));
  UINT32 InCount = (Hwp >> 18) & 31, Total = (Hwp >> 12) & 63;
  for (I = 2; I < USB_MAX_EP; ++I) if (Enabled & ((UINT32)1 << I)) {
    if (Total < InCount || I / 2 >= ((I & 1) ? InCount : Total - InCount)) {
      Status = EFI_UNSUPPORTED; goto Out;
    }
  }
  mVid = Info->DeviceDescriptor->IdVendor; mPid = Info->DeviceDescriptor->IdProduct;
  mConfigValue = Config->ConfigDescriptor->ConfigurationValue;
  Status = Command(0, 9, 0, 0, 0);
  if (EFI_ERROR(Status)) goto Fail;
  for (I = 0; I < USB_MAX_EP; ++I) if (Enabled & ((UINT32)1 << I)) {
    mEndpoints[I].Enabled = TRUE;
    Status = I < 2 ? ConfigureEndpoint((UINT8)I, FALSE) : EFI_SUCCESS;
    if (EFI_ERROR(Status)) goto Fail;
  }
  MmioWrite32(USB_DALEPENA, 3);
  MmioWrite32(USB_DEVTEN, BIT0 | BIT1 | BIT2 | BIT4 | BIT6 | BIT9 | BIT11);
  mConfigured = TRUE;
  Status = Setup();
  if (EFI_ERROR(Status)) goto Fail;
  MemoryFence(); MmioOr32(USB_DCTL, BIT31);
  Status = CrUsbWait(USB_DSTS, BIT22, 0, 100000);
  if (EFI_ERROR(Status)) goto Fail;
  goto Out;
Fail:
  mFault = Status;
Out:
  EfiReleaseLock(&mLock); return Status;
}
STATIC BOOLEAN ValidEndpoint(UINT8 Index, EFI_USBFN_ENDPOINT_DIRECTION Direction)
{ return Index < 16 && (Direction == EfiUsbEndpointDirectionHostIn || Direction == EfiUsbEndpointDirectionHostOut); }
STATIC EFI_STATUS EFIAPI Transfer(EFI_USBFN_IO_PROTOCOL *This, UINT8 Index,
  EFI_USBFN_ENDPOINT_DIRECTION Direction, UINTN *Size, VOID *Buffer)
{
  EFI_STATUS Status;
  UINT8 Ep = Index * 2 + (UINT8)Direction, Type;
  UINTN Bytes, Packet;
  USB_ENDPOINT *E;
  BOOLEAN Zlp;
  if (This != &gCrUsbFunctionIo || Size == NULL || !ValidEndpoint(Index, Direction) ||
      (*Size != 0 && Buffer == NULL) || *Size > USB_MAX_TRANSFER ||
      (*Size != 0 && (UINTN)Buffer > MAX_UINTN - *Size)) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock); E = &mEndpoints[Ep];
  Status = PollLocked();
  if (EFI_ERROR(Status)) goto Out;
  if (!mAvailable || !mStarted || !mConfigured || mRestartControl || mSpeed == UsbBusSpeedUnknown ||
      !E->Enabled || E->Active || E->Pending || E->Stalled || E->Ending) {
    Status = EFI_NOT_READY; goto Out;
  }
  if (Index != 0 && !mBulkConfigured) { Status = EFI_NOT_READY; goto Out; }
  Type = Index == 0 ? mPhase == 1 ? 5 : mControlLength == 0 ? 3 : 4 : 1;
  if (Index == 0 && (mPhase == 0 || mControlAuto || (mPhase == 1 && Direction != mDataDirection) ||
      (mPhase == 1 && (*Size > mControlLength || (Direction == 0 && *Size < mControlLength))) ||
      (mPhase == 2 && (Direction != mStatusDirection || *Size != 0)))) {
    Status = EFI_INVALID_PARAMETER; goto Out;
  }
  Packet = Index == 0 || mSpeed == UsbBusSpeedFull ? 64 : 512;
  Bytes = Direction == 0 ? ALIGN_VALUE(*Size == 0 ? Packet : *Size, Packet) : *Size;
  Status = E->Data.Cpu == NULL ? CrUsbDmaAllocate(&E->Data, USB_MAX_TRANSFER + 512) : EFI_SUCCESS;
  if (EFI_ERROR(Status)) { mFault = Status; goto Out; }
  if (Direction == 1 && *Size != 0) CopyMem(E->Data.Cpu, Buffer, *Size);
  E->Requested = *Size; E->Buffer = Buffer; E->Internal = FALSE; E->Pending = TRUE;
  Zlp = Direction == 1 && *Size != 0 && *Size % Packet == 0 &&
          (Index == 0 ? *Size < mControlLength : E->Zlp);
  Status = Index == 0 && mPhase == 2 ? StartStatus() : Submit(Ep, Bytes, Type, Zlp);
Out:
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI EventHandler(EFI_USBFN_IO_PROTOCOL *This, EFI_USBFN_MESSAGE *Message,
  UINTN *Size, EFI_USBFN_MESSAGE_PAYLOAD *Payload)
{
  EFI_STATUS Status;
  if (This != &gCrUsbFunctionIo || Message == NULL || Size == NULL || (*Size != 0 && Payload == NULL))
    return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock); *Message = EfiUsbMsgNone;
  if (mFrozen) { EfiReleaseLock(&mLock); return EFI_NOT_READY; }
  Status = PollLocked();
  if (mMessageCount != 0) {
    USB_MESSAGE *Entry = &mMessages[mMessageHead];
    if (*Size < Entry->Size || (Entry->Size != 0 && Payload == NULL)) {
      *Size = Entry->Size; Status = EFI_BUFFER_TOO_SMALL;
    } else {
      *Message = Entry->Message; *Size = Entry->Size;
      if (*Size != 0) CopyMem(Payload, &Entry->Payload, *Size);
      mMessageHead = (mMessageHead + 1) % ARRAY_SIZE(mMessages); --mMessageCount;
      Status = EFI_SUCCESS;
    }
  } else *Size = 0;
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI Abort(EFI_USBFN_IO_PROTOCOL *This, UINT8 Index, EFI_USBFN_ENDPOINT_DIRECTION Direction)
{
  UINT8 Ep = Index * 2 + (UINT8)Direction;
  UINT32 I;
  EFI_STATUS Status = EFI_SUCCESS;
  if (This != &gCrUsbFunctionIo || !ValidEndpoint(Index, Direction)) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock);
  if (mFrozen || !mStarted || !mEndpoints[Ep].Enabled) { Status = EFI_NOT_READY; goto Out; }
  if (mEndpoints[Ep].Active) {
    Status = End(Ep);
    if (EFI_ERROR(Status)) goto Out;
    for (I = 0; I < 100000; I += 10) {
      Status = PollLocked();
      if (EFI_ERROR(Status) || mEndpoints[Ep].EndDone) break;
      MicroSecondDelay(10);
    }
    if (I == 100000) { mFault = Status = EFI_TIMEOUT; goto Out; }
  }
  Complete(Ep, UsbTransferStatusAborted);
Out:
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI GetStall(EFI_USBFN_IO_PROTOCOL *This, UINT8 Index,
  EFI_USBFN_ENDPOINT_DIRECTION Direction, BOOLEAN *State)
{
  if (This != &gCrUsbFunctionIo || State == NULL || !ValidEndpoint(Index, Direction)) return EFI_INVALID_PARAMETER;
  *State = mEndpoints[Index * 2 + Direction].Stalled; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SetStall(EFI_USBFN_IO_PROTOCOL *This, UINT8 Index,
  EFI_USBFN_ENDPOINT_DIRECTION Direction, BOOLEAN *State)
{
  EFI_STATUS Status;
  UINT8 Ep = Index * 2 + (UINT8)Direction;
  if (This != &gCrUsbFunctionIo || State == NULL || !ValidEndpoint(Index, Direction)) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock);
  if (mFrozen || !mStarted || !mEndpoints[Ep].Enabled || mEndpoints[Ep].Active) Status = EFI_NOT_READY;
  else {
    if (Index == 0 && *State) {
      if (mEndpoints[0].Active || mEndpoints[1].Active) Status = EFI_NOT_READY;
      else {
        Status = StallControl();
      }
    } else {
      Status = Command(Ep, *State ? 4 : 5, 0, 0, 0);
      if (!EFI_ERROR(Status)) mEndpoints[Ep].Stalled = *State;
    }
  }
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI MaxPacket(EFI_USBFN_IO_PROTOCOL *This, EFI_USB_ENDPOINT_TYPE Type,
  EFI_USB_BUS_SPEED Speed, UINT16 *Packet)
{
  if (This != &gCrUsbFunctionIo || Packet == NULL || (Type != UsbEndpointControl && Type != UsbEndpointBulk) ||
      Speed == UsbBusSpeedLow || (INTN)Speed < 0 || Speed > UsbBusSpeedHigh) return EFI_INVALID_PARAMETER;
  *Packet = Type == UsbEndpointControl || Speed == UsbBusSpeedFull ? 64 : 512;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI Detect(EFI_USBFN_IO_PROTOCOL *This, EFI_USBFN_PORT_TYPE *Type)
{
  if (This != &gCrUsbFunctionIo || Type == NULL) return EFI_INVALID_PARAMETER;
  *Type = EfiUsbUnknownPort;
  return mAvailable ? EFI_SUCCESS : EFI_NOT_READY; /* PD state does not establish BC1.2 port type. */
}
STATIC EFI_STATUS EFIAPI DeviceInfo(EFI_USBFN_IO_PROTOCOL *This, EFI_USBFN_DEVICE_INFO_ID Id,
  UINTN *Size, VOID *Buffer)
{
  if (This != &gCrUsbFunctionIo || Size == NULL || Id == EfiUsbDeviceInfoUnknown ||
      (*Size != 0 && Buffer == NULL)) return EFI_INVALID_PARAMETER;
  return EFI_UNSUPPORTED; /* Identity belongs to the selected USB function client. */
}
STATIC EFI_STATUS EFIAPI VendorProduct(EFI_USBFN_IO_PROTOCOL *This, UINT16 *Vid, UINT16 *Pid)
{
  if (This != &gCrUsbFunctionIo || Vid == NULL || Pid == NULL) return EFI_INVALID_PARAMETER;
  if (mVid == 0) return EFI_NOT_FOUND;
  *Vid = mVid; *Pid = mPid; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI MaxTransfer(EFI_USBFN_IO_PROTOCOL *This, UINTN *Size)
{
  if (This != &gCrUsbFunctionIo || Size == NULL) return EFI_INVALID_PARAMETER;
  *Size = USB_MAX_TRANSFER; return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI AllocateTransfer(EFI_USBFN_IO_PROTOCOL *This, UINTN Size, VOID **Buffer)
{
  EFI_STATUS Status;
  EFI_PHYSICAL_ADDRESS Address;
  UINTN I;
  if (This != &gCrUsbFunctionIo || Buffer == NULL || Size == 0 || Size > USB_MAX_TRANSFER)
    return EFI_INVALID_PARAMETER;
  *Buffer = NULL;
  EfiAcquireLock(&mLock);
  if (mFrozen) { Status = EFI_NOT_READY; goto Out; }
  for (I = 0; I < ARRAY_SIZE(mBuffers); ++I) if (mBuffers[I].Cpu == NULL) break;
  if (I == ARRAY_SIZE(mBuffers)) { Status = EFI_OUT_OF_RESOURCES; goto Out; }
  Status = gBS->AllocatePages(AllocateAnyPages, EfiBootServicesData, EFI_SIZE_TO_PAGES(Size), &Address);
  if (EFI_ERROR(Status)) goto Out;
  mBuffers[I].Cpu = *Buffer = (VOID *)(UINTN)Address; mBuffers[I].Pages = EFI_SIZE_TO_PAGES(Size);
  ZeroMem(*Buffer, EFI_PAGES_TO_SIZE(mBuffers[I].Pages));
Out:
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI FreeTransfer(EFI_USBFN_IO_PROTOCOL *This, VOID *Buffer)
{
  EFI_STATUS Status = EFI_INVALID_PARAMETER;
  UINTN I, J, Base, End;
  if (This != &gCrUsbFunctionIo || Buffer == NULL) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock);
  if (mFrozen) { Status = EFI_NOT_READY; goto Out; }
  for (I = 0; I < ARRAY_SIZE(mBuffers); ++I) if (mBuffers[I].Cpu == Buffer) {
    Base = (UINTN)Buffer; End = Base + EFI_PAGES_TO_SIZE(mBuffers[I].Pages);
    for (J = 0; J < USB_MAX_EP; ++J) if ((mEndpoints[J].Active || mEndpoints[J].Pending) &&
        (UINTN)mEndpoints[J].Buffer >= Base && (UINTN)mEndpoints[J].Buffer < End) {
      Status = EFI_ACCESS_DENIED; goto Out;
    }
    for (J = 0; J < mMessageCount; ++J) {
      USB_MESSAGE *Entry = &mMessages[(mMessageHead + J) % ARRAY_SIZE(mMessages)];
      if ((Entry->Message == EfiUsbMsgEndpointStatusChangedRx || Entry->Message == EfiUsbMsgEndpointStatusChangedTx) &&
          (UINTN)Entry->Payload.utr.Buffer >= Base && (UINTN)Entry->Payload.utr.Buffer < End) {
        Status = EFI_ACCESS_DENIED; goto Out;
      }
    }
    Status = gBS->FreePages((EFI_PHYSICAL_ADDRESS)(UINTN)Buffer, mBuffers[I].Pages);
    if (!EFI_ERROR(Status)) ZeroMem(&mBuffers[I], sizeof(mBuffers[I]));
    break;
  }
Out:
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI SetPolicy(EFI_USBFN_IO_PROTOCOL *This, UINT8 Index,
  EFI_USBFN_ENDPOINT_DIRECTION Direction, EFI_USBFN_POLICY_TYPE Policy, UINTN Size, VOID *Buffer)
{
  EFI_STATUS Status;
  if (This != &gCrUsbFunctionIo || Index == 0 || !ValidEndpoint(Index, Direction) || Buffer == NULL)
    return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mLock);
  if (mFrozen || mStarted) Status = EFI_NOT_READY;
  else if (Policy != EfiUsbPolicyZeroLengthTermination || Size != sizeof(BOOLEAN)) Status = EFI_UNSUPPORTED;
  else { mEndpoints[Index * 2 + Direction].Zlp = *(BOOLEAN *)Buffer; Status = EFI_SUCCESS; }
  EfiReleaseLock(&mLock); return Status;
}
STATIC EFI_STATUS EFIAPI GetPolicy(EFI_USBFN_IO_PROTOCOL *This, UINT8 Index,
  EFI_USBFN_ENDPOINT_DIRECTION Direction, EFI_USBFN_POLICY_TYPE Policy, UINTN *Size, VOID *Buffer)
{
  UINTN Needed, Maximum = USB_MAX_TRANSFER;
  BOOLEAN Value;
  if (This != &gCrUsbFunctionIo || Index == 0 || !ValidEndpoint(Index, Direction) || Size == NULL)
    return EFI_INVALID_PARAMETER;
  if (Policy == EfiUsbPolicyMaxTransactionSize) Needed = sizeof(Maximum);
  else if (Policy == EfiUsbPolicyZeroLengthTerminationSupport || Policy == EfiUsbPolicyZeroLengthTermination)
    Needed = sizeof(Value);
  else return EFI_UNSUPPORTED;
  if (*Size < Needed || Buffer == NULL) { *Size = Needed; return EFI_BUFFER_TOO_SMALL; }
  *Size = Needed;
  if (Policy == EfiUsbPolicyMaxTransactionSize) CopyMem(Buffer, &Maximum, Needed);
  else {
    Value = Policy == EfiUsbPolicyZeroLengthTerminationSupport ? TRUE : mEndpoints[Index * 2 + Direction].Zlp;
    CopyMem(Buffer, &Value, Needed);
  }
  return EFI_SUCCESS;
}
EFI_USBFN_IO_PROTOCOL gCrUsbFunctionIo = {
  EFI_USBFN_IO_PROTOCOL_REVISION, Detect, Configure, MaxPacket, DeviceInfo, VendorProduct,
  Abort, GetStall, SetStall, EventHandler, Transfer, MaxTransfer, AllocateTransfer, FreeTransfer,
  StartController, StopController, SetPolicy, GetPolicy
};
