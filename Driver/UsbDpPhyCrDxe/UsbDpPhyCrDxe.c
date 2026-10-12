/** Native retained dependency adapter for the shared SM8450-HDK PHY.
    SPDX-License-Identifier: GPL-2.0-only */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UsbDpPhyCrLib.h>
#include <oskal/cr_atomic.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Protocol/EFIUsbDpPhyCrProtocol.h>
#include <Protocol/EFIMdssHandoffProtocol.h>
#include "Fsa4480.h"

#define QMP 0x088E8000U
#define HS 0x088E3000U
STATIC EFI_CLOCK_CR_PROTOCOL *mClock;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmh;
STATIC EFI_USB_DP_PHY_CR_PROTOCOL mProtocol;
STATIC EFI_LOCK mLock;
STATIC CR_FSA4480 mFsa;
STATIC CR_USB_DP_PHY mPhy;
STATIC UINT8 mClientOwner[UsbDpPhyClientCount];
STATIC EFI_MDSS_HANDOFF_PROTOCOL *mHandoff;

STATIC CR_STATUS NativeReady(VOID *Context)
{
  if (Context != &mProtocol || mClock == NULL || mRpmh == NULL || mFsa.Bus == NULL ||
      mClock->SetClock == NULL || mClock->SetReset == NULL ||
      mRpmh->AcquireVreg == NULL || mRpmh->RpmhEnableVreg == NULL ||
      mFsa.Bus->StartRequest == NULL || mFsa.Bus->SetBusFrequency == NULL)
    return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS NativeRead(VOID *Context, CR_USB_DP_PHY_WINDOW Window,
  UINT32 Offset, UINT32 *Value)
{
  if (NativeReady(Context) != CR_SUCCESS || Value == NULL ||
      (UINT32)Window >= CrUsbDpPhyWindows || (Offset & 3) ||
      Offset > (Window == CrUsbDpPhyQmp ? CR_USB_DP_PHY_QMP_BYTES : CR_USB_DP_PHY_HS_BYTES) - 4)
    return CR_INVALID_PARAMETER;
  *Value = MmioRead32((Window == CrUsbDpPhyQmp ? QMP : HS) + Offset);
  return CR_SUCCESS;
}
STATIC CR_STATUS NativeWrite(VOID *Context, CR_USB_DP_PHY_WINDOW Window,
  UINT32 Offset, UINT32 Value)
{
  if (NativeReady(Context) != CR_SUCCESS || (UINT32)Window >= CrUsbDpPhyWindows ||
      (Offset & 3) || Offset >
      (Window == CrUsbDpPhyQmp ? CR_USB_DP_PHY_QMP_BYTES : CR_USB_DP_PHY_HS_BYTES) - 4)
    return CR_INVALID_PARAMETER;
  MmioWrite32((Window == CrUsbDpPhyQmp ? QMP : HS) + Offset, Value);
  return CR_SUCCESS;
}
STATIC CR_STATUS NativeClock(VOID *Context, CONST CHAR8 *Name, UINT32 Rate,
  BOOLEAN On, BOOLEAN *Owned)
{
  EFI_STATUS S;
  if (NativeReady(Context) != CR_SUCCESS || Owned == NULL) return CR_NOT_READY;
  if (On) return CrAcquireClock(mClock, "gcc", Name, Rate, Owned);
  S = mClock->SetClock(mClock, "gcc", Name, 0, FALSE);
  if (S == EFI_SUCCESS) *Owned = FALSE;
  return S;
}
STATIC CR_STATUS NativeRail(VOID *Context, CONST CHAR8 *Name, UINT32 Mv,
  UINT8 Mode, BOOLEAN On, BOOLEAN *Owned)
{
  EFI_STATUS S;
  if (NativeReady(Context) != CR_SUCCESS || Owned == NULL) return CR_NOT_READY;
  if (On) return mRpmh->AcquireVreg(mRpmh, Name, Mv, Mode, Owned);
  S = mRpmh->RpmhEnableVreg(mRpmh, Name, FALSE);
  if (S == EFI_SUCCESS) *Owned = FALSE;
  return S;
}
STATIC CR_STATUS NativeReset(VOID *Context, CONST CHAR8 *Name, BOOLEAN Assert)
{
  if (NativeReady(Context) != CR_SUCCESS) return CR_NOT_READY;
  return mClock->SetReset(mClock, "gcc", Name, Assert);
}
STATIC CR_STATUS NativeSwitchRead(VOID *Context, UINT8 Reg, UINT8 *Value)
{
  if (NativeReady(Context) != CR_SUCCESS) return CR_NOT_READY;
  return CrFsaRead(&mFsa, Reg, Value);
}
STATIC CR_STATUS NativeSwitchWrite(VOID *Context, UINT8 Reg, UINT8 Value, BOOLEAN *Dispatched)
{
  if (NativeReady(Context) != CR_SUCCESS) return CR_NOT_READY;
  return CrFsaWrite(&mFsa, Reg, Value, Dispatched);
}
STATIC EFI_STATUS InitializeCore(VOID)
{
  CONST CR_USB_DP_PHY_CONFIG Config = {CR_USB_DP_PHY_REVISION, CR_USB_DP_PHY_SOC,
    CR_USB_DP_PHY_HDK, {{QMP, CR_USB_DP_PHY_QMP_BYTES}, {HS, CR_USB_DP_PHY_HS_BYTES}}};
  CONST CR_USB_DP_PHY_IO Io = {&mProtocol, NativeReady, NativeRead, NativeWrite,
    NativeClock, NativeRail, NativeReset, NativeSwitchRead, NativeSwitchWrite};
  return CrUsbDpPhyInit(&mPhy, &Config, &Io);
}
STATIC CR_USB_DP_PHY_ROUTE Convert(CONST EFI_USB_TYPEC_STATE *State)
{
  CR_USB_DP_PHY_ROUTE R = {0};
  R.Orientation = State->Orientation; R.DpLanes = State->DpLanes;
  R.PinAssignment = State->PinAssignment; R.PdReady = State->PdReady;
  R.Connected = State->Connected; R.DpActive = State->DpActive;
  R.Usb3 = State->Usb3; R.LastError = State->LastError;
  return R;
}
/* The UEFI protocol has one fixed consumer slot per USB/DP client; the
   portable engine uses these stable tokens as its exact owner identities.
   Keep the existing TPL_CALLBACK exclusion around its native spin mutex. */
STATIC EFI_STATUS EFIAPI Acquire(EFI_USB_DP_PHY_CR_PROTOCOL *This,
  EFI_USB_DP_PHY_CLIENT Client, CONST EFI_USB_TYPEC_STATE *State,
  BOOLEAN SuperSpeed, BOOLEAN *Owned)
{
  EFI_STATUS S;
  if (Owned != NULL) *Owned = FALSE;
  if (This != &mProtocol || (UINT32)Client >= UsbDpPhyClientCount || State == NULL || Owned == NULL)
    return EFI_INVALID_PARAMETER;
  CR_USB_DP_PHY_ROUTE R = Convert(State);
  if (CrAtomicLoad32(&mPhy.Frozen)) return EFI_ACCESS_DENIED;
  EfiAcquireLock(&mLock);
  S = CrUsbDpPhyAcquire(&mPhy, &mClientOwner[Client], (CR_USB_DP_PHY_CLIENT)Client, &R, SuperSpeed, Owned);
  if (S == EFI_SUCCESS) DEBUG((DEBUG_WARN, "UsbDpPhyCr: shared common orientation=%u DP=%u lanes=%u USBss=%u\n",
    mPhy.Route.Orientation, mPhy.Route.DpActive, mPhy.Route.DpLanes, mPhy.UsbSs));
  EfiReleaseLock(&mLock);
  return S;
}
STATIC EFI_STATUS EFIAPI Release(EFI_USB_DP_PHY_CR_PROTOCOL *This, EFI_USB_DP_PHY_CLIENT Client)
{
  if (This != &mProtocol || (UINT32)Client >= UsbDpPhyClientCount) return EFI_INVALID_PARAMETER;
  if (CrAtomicLoad32(&mPhy.Frozen)) return EFI_ACCESS_DENIED;
  EfiAcquireLock(&mLock);
  EFI_STATUS S = CrUsbDpPhyRelease(&mPhy, &mClientOwner[Client], (CR_USB_DP_PHY_CLIENT)Client);
  EfiReleaseLock(&mLock);
  return S;
}
STATIC EFI_STATUS EFIAPI PowerDp(EFI_USB_DP_PHY_CR_PROTOCOL *This, UINT32 Rate, UINT8 Lanes)
{
  if (This != &mProtocol) return EFI_INVALID_PARAMETER;
  if (CrAtomicLoad32(&mPhy.Frozen)) return EFI_ACCESS_DENIED;
  EfiAcquireLock(&mLock);
  EFI_STATUS S = CrUsbDpPhyPowerDp(&mPhy, &mClientOwner[UsbDpPhyDp], Rate, Lanes);
  if (EFI_ERROR(S)) DEBUG((DEBUG_ERROR,"UsbDpPhyCr: DP %r window=%u offset=%x value=%x\n",
    S,mPhy.LastWindow,mPhy.LastOffset,mPhy.LastValue));
  EfiReleaseLock(&mLock);
  return S;
}
STATIC EFI_STATUS EFIAPI Levels(EFI_USB_DP_PHY_CR_PROTOCOL *This, UINT8 V, UINT8 Pre)
{
  if (This != &mProtocol) return EFI_INVALID_PARAMETER;
  if (CrAtomicLoad32(&mPhy.Frozen)) return EFI_ACCESS_DENIED;
  EfiAcquireLock(&mLock);
  EFI_STATUS S = CrUsbDpPhySetDpLevels(&mPhy, &mClientOwner[UsbDpPhyDp], V, Pre);
  EfiReleaseLock(&mLock);
  return S;
}

/* The coordinator calls this before SMMU revocation. There is no deferred
   work in this driver; the same lock admits every mutating entry point.
   Preserve all hardware and exact ownership journals for the OS reader. */
STATIC EFI_STATUS EFIAPI HandoffFreeze(VOID *Context)
{
  if (Context != &mProtocol) return EFI_INVALID_PARAMETER;
  EFI_TPL OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  CR_STATUS Closed = CrUsbDpPhyFreeze(&mPhy);
  /* A preempted lower-TPL operation may still own its journal. Admission
     stays closed, but do not certify its snapshot while it can mutate. */
  EFI_STATUS Status = (Closed != CR_SUCCESS || mLock.Lock == EfiLockAcquired) ? EFI_NOT_READY : EFI_SUCCESS;
  gBS->RestoreTPL(OldTpl);
  return Status;
}

STATIC VOID SnapshotRail(CR_MDSS_HANDOFF_PARTICIPANT *S, CONST CR_USB_DP_PHY_RAIL *R)
{
  CR_MDSS_NATIVE_RAIL *Out;
  if (!R->Owned) return;
  Out = &S->Rails[S->RailCount++];
  Out->Owner = CR_MDSS_NATIVE_PHY;
  Out->Flags = CR_MDSS_NATIVE_OWNED;
  if (EFI_ERROR(R->Status)) Out->Flags |= CR_MDSS_NATIVE_RETAINED_ERROR;
  CopyMem(Out->Name, R->Name, AsciiStrLen(R->Name) + 1);
  Out->Millivolts = R->Millivolts; Out->Mode = R->Mode;
}

STATIC EFI_STATUS EFIAPI HandoffSnapshot(VOID *Context,
  CR_MDSS_HANDOFF_PARTICIPANT *S, VOID **Map)
{
  EFI_STATUS Status = EFI_SUCCESS;
  UINTN I;
  if (Context != &mProtocol || S == NULL || Map == NULL) return EFI_INVALID_PARAMETER;
  EFI_TPL OldTpl = gBS->RaiseTPL(TPL_NOTIFY);
  if (!CrAtomicLoad32(&mPhy.Frozen) || CrAtomicLoad32(&mPhy.Active) || mLock.Lock == EfiLockAcquired) {
    gBS->RestoreTPL(OldTpl); return EFI_NOT_READY;
  }
  ZeroMem(S, sizeof(*S));
  S->Owner = CR_MDSS_NATIVE_PHY;
  *Map = NULL;
  if (EFI_ERROR(mPhy.CommonStatus)) Status = mPhy.CommonStatus;
  for (I = 0; I < UsbDpPhyClientCount; ++I)
    if (mPhy.Owner[I] && Status == EFI_SUCCESS && EFI_ERROR(mPhy.ClientStatus[I])) Status = mPhy.ClientStatus[I];
  for (I = 0; I < ARRAY_SIZE(mPhy.Clocks); ++I) if (mPhy.Clocks[I].Owned) {
    CR_MDSS_NATIVE_CLOCK *Out = &S->Clocks[S->ClockCount++];
    Out->Owner = CR_MDSS_NATIVE_PHY; Out->Flags = CR_MDSS_NATIVE_OWNED;
    if (EFI_ERROR(mPhy.Clocks[I].Status)) {
      Out->Flags |= CR_MDSS_NATIVE_RETAINED_ERROR;
      if (Status == EFI_SUCCESS) Status = mPhy.Clocks[I].Status;
    }
    CopyMem(Out->Controller, "gcc", sizeof("gcc"));
    CopyMem(Out->Name, mPhy.Clocks[I].Name, AsciiStrLen(mPhy.Clocks[I].Name) + 1);
    Out->RateHz = mPhy.Clocks[I].Rate;
  }
  for (I = 0; I < CR_USB_DP_PHY_RAILS; ++I) {
    SnapshotRail(S, &mPhy.Rails[I]);
    if (mPhy.Rails[I].Owned && Status == EFI_SUCCESS && EFI_ERROR(mPhy.Rails[I].Status))
      Status = mPhy.Rails[I].Status;
  }
  if (mPhy.Owner[UsbDpPhyUsb] || mPhy.Owner[UsbDpPhyDp] || S->ClockCount || S->RailCount ||
      mPhy.CommonTouched || mPhy.UsbTouched || mPhy.DpTouched || mPhy.SwitchTouched) {
    CR_MDSS_NATIVE_PHY_STATE *P = &S->Phy;
    P->Flags = CR_MDSS_NATIVE_OWNED;
    P->State = CR_MDSS_NATIVE_LIVE;
    if (EFI_ERROR(Status) || !mPhy.CommonReady ||
        (mPhy.Owner[UsbDpPhyUsb] && !mPhy.Ready[UsbDpPhyUsb]) ||
        (mPhy.Owner[UsbDpPhyDp] && !mPhy.Ready[UsbDpPhyDp])) {
      P->State = CR_MDSS_NATIVE_UNCERTAIN;
      /* A missing readiness witness is not a successful retained operation. */
      if (Status == EFI_SUCCESS) Status = EFI_NOT_READY;
    }
    if (EFI_ERROR(Status)) P->Flags |= CR_MDSS_NATIVE_RETAINED_ERROR;
    P->Status = Status;
    P->UsbOwners = mPhy.Owner[UsbDpPhyUsb] ? 1 : 0;
    P->DpOwners = mPhy.Owner[UsbDpPhyDp] ? 1 : 0;
    P->Base = QMP; P->Bytes = 0x4000;
    if (mPhy.CommonReady) {
      P->Orientation = mPhy.Route.Orientation;
      P->LaneCount = mPhy.DpRate ? mPhy.DpLanes : mPhy.Route.DpLanes;
    }
    P->LinkRate = mPhy.DpRate;
    if (mPhy.CommonReady) P->ReadyMask |= CR_MDSS_NATIVE_PHY_COMMON;
    if (mPhy.Ready[UsbDpPhyUsb]) P->ReadyMask |= CR_MDSS_NATIVE_PHY_USB;
    if (mPhy.Ready[UsbDpPhyDp]) P->ReadyMask |= CR_MDSS_NATIVE_PHY_DP;
    if (mPhy.SwitchKnown) P->ReadyMask |= CR_MDSS_NATIVE_PHY_FSA;
    if (mPhy.Owner[UsbDpPhyUsb] && mPhy.Ready[UsbDpPhyUsb] && mPhy.UsbSs)
      P->ReadyMask |= CR_MDSS_NATIVE_PHY_USB_SS;
    if (mPhy.CommonTouched) P->TouchedMask |= CR_MDSS_NATIVE_PHY_COMMON;
    if (mPhy.UsbTouched) P->TouchedMask |= CR_MDSS_NATIVE_PHY_USB;
    if (mPhy.DpTouched) P->TouchedMask |= CR_MDSS_NATIVE_PHY_DP;
    if (mPhy.SwitchTouched) P->TouchedMask |= CR_MDSS_NATIVE_PHY_FSA;
  }
  gBS->RestoreTPL(OldTpl);
  return EFI_SUCCESS;
}

EFI_STATUS EFIAPI UsbDpPhyCrEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EFI_HANDLE *Handles;
  UINTN Count, I;
  (VOID)SystemTable;
  Status = gBS->LocateProtocol(&gEfiMdssHandoffProtocolGuid, NULL, (VOID **)&mHandoff);
  if (EFI_ERROR(Status)) return Status;
  if (mHandoff == NULL || mHandoff->Revision < EFI_MDSS_HANDOFF_PROTOCOL_REVISION ||
      mHandoff->SetParticipant == NULL) return EFI_UNSUPPORTED;
  Status = gBS->LocateProtocol(&gEfiClockCrProtocolGuid, NULL, (VOID **)&mClock);
  if (EFI_ERROR(Status)) return Status;
  Status = gBS->LocateProtocol(&gEfiRpmhCrProtocolGuid, NULL, (VOID **)&mRpmh);
  if (EFI_ERROR(Status)) return Status;
  if (mClock == NULL || mClock->SetClock == NULL || mClock->SetReset == NULL ||
      mRpmh == NULL || mRpmh->AcquireVreg == NULL || mRpmh->RpmhEnableVreg == NULL)
    return EFI_UNSUPPORTED;
  Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiI2cCrProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR(Status)) return Status;
  for (I = 0; I < Count; ++I) {
    EFI_I2C_CR_PROTOCOL *Bus;
    Status = gBS->HandleProtocol(Handles[I], &gEfiI2cCrProtocolGuid, (VOID **)&Bus);
    if (!EFI_ERROR(Status) && Bus != NULL && (UINT32)Bus->Instance == 6 && Bus->StartRequest != NULL) {
      mFsa.Bus = Bus; break;
    }
  }
  FreePool(Handles);
  if (mFsa.Bus == NULL) return EFI_NOT_FOUND;
  EfiInitializeLock(&mLock, TPL_CALLBACK);
  Status = InitializeCore();
  if (Status != EFI_SUCCESS) return Status;
  mProtocol.Revision = EFI_USB_DP_PHY_CR_PROTOCOL_REVISION;
  mProtocol.Acquire = Acquire; mProtocol.Release = Release;
  mProtocol.PowerDp = PowerDp; mProtocol.SetDpLevels = Levels;
  Status = gBS->InstallMultipleProtocolInterfaces(&ImageHandle,
           &gEfiUsbDpPhyCrProtocolGuid, &mProtocol, NULL);
  if (EFI_ERROR(Status)) return Status;
  Status = mHandoff->SetParticipant(mHandoff, CR_MDSS_NATIVE_PHY, &mProtocol,
                                    HandoffFreeze, HandoffSnapshot);
  if (Status != EFI_SUCCESS) {
    EFI_STATUS Cleanup;
    if (!EFI_ERROR(Status)) Status = EFI_NOT_READY;
    Cleanup = gBS->UninstallMultipleProtocolInterfaces(ImageHandle,
                         &gEfiUsbDpPhyCrProtocolGuid, &mProtocol, NULL);
    if (EFI_ERROR(Cleanup)) {
      /* An installed method still points into this image. Keep it resident;
         the missing participant makes the coordinator decline publication. */
      DEBUG((DEBUG_ERROR, "UsbDpPhyCr: handoff register %r, retained protocol %r\n", Status, Cleanup));
      return EFI_SUCCESS;
    }
  }
  return Status;
}
