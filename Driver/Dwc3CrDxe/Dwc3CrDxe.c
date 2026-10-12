/** Native SM8450 DWC3 Type-C role monitor, xHCI and USB function integration.
    SPDX-License-Identifier: GPL-2.0-only */
#include "Dwc3.h"
#include "Dwc3Handoff.h"
#include <Library/Dwc3CrLib.h>
#include <Protocol/Usb2HcFreeze.h>
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/Dwc3PowerCrLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/NonDiscoverableDeviceRegistrationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/EFIDmaIdentityCrProtocol.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/EFIUsbDpPhyCrProtocol.h>

STATIC EFI_USB_TYPEC_CR_PROTOCOL *mTypeC;
STATIC EFI_USB_DP_PHY_CR_PROTOCOL *mPhy;
STATIC EFI_CLOCK_CR_PROTOCOL *mClock;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mIcb;
STATIC EFI_SMMU_CR_PROTOCOL *mSmmu;
STATIC EFI_HANDLE mHost, mFunction;
STATIC EFI_EVENT mTimer, mExit;
STATIC EFI_USB_TYPEC_STATE mRoute;
STATIC EFI_USB_TYPEC_DATA_ROLE mRole;
STATIC BOOLEAN mPhyOwned, mCoreReady, mHostRegistered, mActive, mBusy, mExiting;
STATIC UINT32 mRetryTicks, mCleanupRetryTicks;
STATIC EFI_USB_TYPEC_STATE mRetryRoute;
STATIC BOOLEAN mHaveRetryRoute;
STATIC INTERCONNECT_PATH_HANDLE mPaths[2];
STATIC CR_DWC3_POWER mPower;
STATIC EFI_DMA_IDENTITY_CR_PROTOCOL mIdentity = {
  EFI_DMA_IDENTITY_CR_PROTOCOL_REVISION, 0x15000000, CR_USB_BASE, 0x100000, 0
};
STATIC CR_STATUS PowerAccess(VOID *Context) {
  return Context==&mPower && !mExiting && mClock && mIcb ? EFI_SUCCESS : EFI_NOT_READY;
}
STATIC CR_STATUS PowerPath(VOID *Context,UINT32 I,BOOLEAN On,UINT64 Average,UINT64 Peak,BOOLEAN *Owned) {
  (VOID)Context;
  EFI_STATUS S;
  if(On) {
    S=mIcb->AcquirePath(mIcb,"qcom,sm8450-dwc3",I==0?0x6000:0x6002,I==0?0x6001:0x6003,&mPaths[I]);
    *Owned=mPaths[I]!=INTERCONNECT_INVALID_PATH;
    if(S==EFI_SUCCESS && *Owned)S=mIcb->SetBandwidth(mIcb,mPaths[I],Average,Peak);
  } else {
    S=mIcb->ReleasePath(mIcb,mPaths[I]);
    if(S==EFI_SUCCESS){mPaths[I]=INTERCONNECT_INVALID_PATH;*Owned=FALSE;}
  }
  return S;
}
STATIC CR_STATUS PowerDomain(VOID *Context,BOOLEAN On,BOOLEAN *Owned) {
  (VOID)Context;
  if(On)return CrAcquireGdsc(mClock,"gcc","usb30_prim_gdsc",Owned);
  EFI_STATUS S=mClock->SetGdsc(mClock,"gcc","usb30_prim_gdsc",FALSE);
  if(S==EFI_SUCCESS)*Owned=FALSE;return S;
}
STATIC CR_STATUS PowerClock(VOID *Context,CONST CHAR8 *Name,UINT64 Rate,BOOLEAN On,BOOLEAN *Owned) {
  (VOID)Context;
  if(On)return CrAcquireClock(mClock,"gcc",Name,Rate,Owned);
  EFI_STATUS S=mClock->SetClock(mClock,"gcc",Name,0,FALSE);
  if(S==EFI_SUCCESS)*Owned=FALSE;return S;
}
STATIC CR_STATUS PowerReset(VOID *Context,BOOLEAN Assert) {
  (VOID)Context;return mClock->SetReset(mClock,"gcc","gcc_usb30_prim_bcr",Assert);
}
STATIC CR_STATUS PowerDivider(VOID *Context) {
  (VOID)Context;return mClock->SetDivider(mClock,"gcc","gcc_usb30_prim_mock_utmi_clk",1);
}
STATIC CONST CR_DWC3_POWER_IO mPowerIo={&mPower,PowerAccess,PowerPath,PowerDomain,PowerClock,PowerReset,PowerDivider};
#pragma pack(1)
STATIC struct { VENDOR_DEVICE_PATH Vendor; EFI_DEVICE_PATH_PROTOCOL End; } mFunctionPath = {
  {{HARDWARE_DEVICE_PATH, HW_VENDOR_DP, {sizeof(VENDOR_DEVICE_PATH), 0}},
   {0x93c6fe4a,0xd4fc,0x47a1,{0xa7,0x6e,0x61,0xd5,0x8d,0x0d,0x28,0x0d}}},
  {END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, {sizeof(EFI_DEVICE_PATH_PROTOCOL), 0}}
};
#pragma pack()

STATIC EFI_STATUS RemoveHost(VOID)
{
  EFI_STATUS Status;
  NON_DISCOVERABLE_DEVICE *Device;
  EFI_DEVICE_PATH_PROTOCOL *Path;
  if (mHost == NULL) return EFI_SUCCESS;
  if (mHostRegistered) {
    /* xHCI Stop must never free its rings while hardware is still running. */
    Status = CrUsbCoreHalt(UsbTypeCHost);
    if (EFI_ERROR(Status)) return Status;
    Status = gBS->DisconnectController(mHost, NULL, NULL);
    if (EFI_ERROR(Status) && Status != EFI_NOT_FOUND) return Status;
    Status = CrUsbCoreHalt(UsbTypeCHost);
    if (EFI_ERROR(Status)) return Status;
    Status = gBS->HandleProtocol(mHost, &gEdkiiNonDiscoverableDeviceProtocolGuid, (VOID **)&Device);
    if (EFI_ERROR(Status)) return Status;
    Status = gBS->HandleProtocol(mHost, &gEfiDevicePathProtocolGuid, (VOID **)&Path);
    if (EFI_ERROR(Status)) return Status;
    Status = gBS->UninstallMultipleProtocolInterfaces(mHost,
               &gEdkiiNonDiscoverableDeviceProtocolGuid, Device, &gEfiDevicePathProtocolGuid, Path, NULL);
    if (EFI_ERROR(Status)) return Status;
    mHostRegistered = FALSE;
    FreePool(Path); FreePool(Device);
  }
  Status = gBS->UninstallMultipleProtocolInterfaces(mHost, &gEfiDmaIdentityCrProtocolGuid, &mIdentity, NULL);
  if (!EFI_ERROR(Status)) mHost = NULL;
  return Status;
}

STATIC EFI_STATUS Stop(VOID)
{
  EFI_STATUS Status;
  mActive = FALSE;
  CrUsbFnSetAvailable(FALSE);
  Status = CrUsbFnStop();
  if (EFI_ERROR(Status)) return Status;
  Status = RemoveHost();
  if (EFI_ERROR(Status)) return Status;
  if (mPower.ResetTouched) {
    if (mCoreReady) {
      Status = CrUsbCoreHalt(mRole);
      if (EFI_ERROR(Status)) return Status;
    }
    Status = CrDwc3PowerQuiesce(&mPower);
    if (EFI_ERROR(Status)) return Status;
    mCoreReady = FALSE;
  }
  if (mPhyOwned) {
    Status = mPhy->Release(mPhy, UsbDpPhyUsb);
    if (EFI_ERROR(Status)) return Status;
    mPhyOwned = FALSE;
  }
  if(mPower.Initialized) {
    Status = CrDwc3PowerRelease(&mPower);
    if (EFI_ERROR(Status)) return Status;
  }
  mRole = UsbTypeCNone;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI HostInit(NON_DISCOVERABLE_DEVICE *Device)
{
  (VOID)Device;
  return !mExiting && mCoreReady && mRole == UsbTypeCHost ? EFI_SUCCESS : EFI_NOT_READY;
}
STATIC EFI_STATUS Start(CONST EFI_USB_TYPEC_STATE *State)
{
  EFI_STATUS Status;
  BOOLEAN SuperSpeed = State->DataRole == UsbTypeCHost && State->Usb3;
  Status = CrDwc3PowerAcquire(&mPower,SuperSpeed);
  if (EFI_ERROR(Status)) return Status;
  Status = mPhy->Acquire(mPhy, UsbDpPhyUsb, State, SuperSpeed, &mPhyOwned);
  if (EFI_ERROR(Status)) return Status;
  mRole = State->DataRole;
  Status = CrUsbCoreInit(mRole, SuperSpeed);
  if (EFI_ERROR(Status)) return Status;
  mCoreReady = TRUE;
  Status = mSmmu->Attach(mSmmu, 0, 0xFFFF, 0);
  if (EFI_ERROR(Status)) return Status;
  mRoute = *State;
  if (mRole == UsbTypeCDevice) {
    CrUsbFnSetAvailable(TRUE);
  } else {
    Status = gBS->InstallMultipleProtocolInterfaces(&mHost, &gEfiDmaIdentityCrProtocolGuid, &mIdentity, NULL);
    if (EFI_ERROR(Status)) return Status;
    Status = RegisterNonDiscoverableMmioDevice(0xD300, NonDiscoverableDeviceTypeXhci,
               NonDiscoverableDeviceDmaTypeNonCoherent, HostInit, &mHost, 1, (UINTN)CR_USB_BASE, (UINTN)0xC000);
    if (EFI_ERROR(Status)) return Status;
    mHostRegistered = TRUE;
    Status = gBS->ConnectController(mHost, NULL, NULL, TRUE);
    if (EFI_ERROR(Status)) return Status;
  }
  mActive = TRUE;
  DEBUG((DEBUG_WARN, "Dwc3Cr: Type-C role=%a superspeed=%u DP-lanes=%u orientation=%u ready\n",
         mRole == UsbTypeCHost ? "host" : "device (USB2 UsbFunctionIo)", SuperSpeed,
         State->DpLanes, State->Orientation));
  return EFI_SUCCESS;
}
STATIC BOOLEAN SameTopology(CONST EFI_USB_TYPEC_STATE *State,
                            CONST EFI_USB_TYPEC_STATE *Route)
{
  return Route->DataRole == State->DataRole && Route->Orientation == State->Orientation &&
         Route->Usb3 == State->Usb3 && Route->DpActive == State->DpActive &&
         Route->DpLanes == State->DpLanes && Route->PinAssignment == State->PinAssignment;
}
STATIC VOID EFIAPI Monitor(EFI_EVENT Event, VOID *Context)
{
  EFI_USB_TYPEC_STATE State;
  EFI_STATUS Status;
  BOOLEAN Wanted;
  (VOID)Event; (VOID)Context;
  if (mBusy || mExiting) return;
  mBusy = TRUE;
  /* The UCSI image can be unloaded from Shell; discover it again before
     calling a callback that might otherwise point into a removed image. */
  mTypeC = NULL;
  Status = gBS->LocateProtocol(&gEfiUsbTypeCCrProtocolGuid, NULL, (VOID **)&mTypeC);
  if (!EFI_ERROR(Status) && mTypeC != NULL &&
      mTypeC->Revision >= EFI_USB_TYPEC_CR_PROTOCOL_REVISION && mTypeC->GetState != NULL)
    Status = mTypeC->GetState(mTypeC, &State);
  else Status = EFI_NOT_READY;
  Wanted = !EFI_ERROR(Status) && State.PdReady && State.Connected && !EFI_ERROR(State.LastError) &&
           (State.DataRole == UsbTypeCHost || State.DataRole == UsbTypeCDevice);
  if (mActive && Wanted && SameTopology(&State, &mRoute)) {
    Status = mRole == UsbTypeCDevice ? CrUsbFnPoll() : EFI_SUCCESS;
    if (!EFI_ERROR(Status)) goto Out;
    DEBUG((DEBUG_ERROR, "Dwc3Cr: function event processing: %r\n", Status));
  }
  if (mCleanupRetryTicks != 0) { --mCleanupRetryTicks; goto Out; }
  Status = Stop();
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Dwc3Cr: cleanup retained: %r\n", Status));
    mCleanupRetryTicks = 100; /* keep resources; retry after one second */
    goto Out;
  }
  if (!Wanted) { mRetryTicks = 0; mHaveRetryRoute = FALSE; goto Out; }
  /* HPD IRQs also increment UCSI Sequence; they must not bypass a failed
     USB role's backoff when its actual topology has stayed the same. */
  if (!mHaveRetryRoute || !SameTopology(&State, &mRetryRoute)) {
    mRetryRoute = State; mHaveRetryRoute = TRUE; mRetryTicks = 0;
  }
  if (mRetryTicks != 0) { --mRetryTicks; goto Out; }
  Status = Start(&State);
  if (EFI_ERROR(Status)) {
    EFI_STATUS Cleanup = Stop();
    DEBUG((DEBUG_ERROR, "Dwc3Cr: role start: %r cleanup: %r\n", Status, Cleanup));
    if (EFI_ERROR(Cleanup)) mCleanupRetryTicks = 100;
    mRetryTicks = 100;
  }
Out:
  mBusy = FALSE;
}
STATIC EFI_STATUS FreezeNative(CR_USB_NATIVE_HANDOFF *R)
{
  EFI_STATUS S,Result=EFI_SUCCESS;
  EDKII_USB2_HC_FREEZE_PROTOCOL *Freeze=NULL;
  EFI_USB2_HC_PROTOCOL *HostController=NULL;
  mExiting=TRUE;
  // Only report the last applied route here. PIL may already have stopped
  // ADSP, so an EBS diagnostic must not initiate another UCSI transaction.
  DEBUG((DEBUG_WARN,
    "Dwc3Cr: EBS cached-route sequence=%u role=%u source=%u orientation=%u DP=%u lanes=%u USB3=%u active=%u core=%u power=%u phy=%u\n",
    mRoute.Sequence,mRole,mRoute.PowerSource,mRoute.Orientation,mRoute.DpActive,
    mRoute.DpLanes,mRoute.Usb3,mActive,mCoreReady,mPower.Ready,mPhyOwned));
  R->FrozenWriters=CR_USBH_MONITOR_WRITER;
  R->ActiveCalls[0]=mBusy?1:0;
  if(!mBusy)R->DrainedWriters|=CR_USBH_MONITOR_WRITER;
  else {R->FailedWriters|=CR_USBH_MONITOR_WRITER;Result=EFI_NOT_READY;}
  S=CrUsbFnFreeze(&R->ActiveCalls[2]);
  if(S==EFI_SUCCESS){R->FrozenWriters|=CR_USBH_FUNCTION_WRITER;R->DrainedWriters|=CR_USBH_FUNCTION_WRITER;}
  else {R->FailedWriters|=CR_USBH_FUNCTION_WRITER;if(Result==EFI_SUCCESS)Result=S;}
  if(mHost==NULL){
    R->FrozenWriters|=CR_USBH_XHC_WRITER;R->DrainedWriters|=CR_USBH_XHC_WRITER;
  } else {
    S=gBS->HandleProtocol(mHost,&gEdkiiUsb2HcFreezeProtocolGuid,(VOID**)&Freeze);
    if(S==EFI_SUCCESS)S=gBS->HandleProtocol(mHost,&gEfiUsb2HcProtocolGuid,(VOID**)&HostController);
    if(S==EFI_SUCCESS && Freeze && Freeze->Revision==EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION &&
       Freeze->Freeze && HostController && Freeze->HostController==HostController){
      S=Freeze->Freeze(Freeze,&R->ActiveCalls[1]);
      // The exact extension always closes admission, even while a callback drains.
      if(S==EFI_SUCCESS || S==EFI_NOT_READY)R->FrozenWriters|=CR_USBH_XHC_WRITER;
      if(S==EFI_SUCCESS && !R->ActiveCalls[1])R->DrainedWriters|=CR_USBH_XHC_WRITER;
      else if(S==EFI_SUCCESS)S=EFI_NOT_READY;
    } else if(S==EFI_SUCCESS)S=EFI_UNSUPPORTED;
    if(S!=EFI_SUCCESS){R->FailedWriters|=CR_USBH_XHC_WRITER;if(Result==EFI_SUCCESS)Result=S;}
  }
  if(Result!=EFI_SUCCESS)return Result;
  // Only the qualified live domain/PHY journal authorizes register access.
  if(!mCoreReady || !mPower.Ready || !mPhyOwned)return EFI_SUCCESS;
  EFI_STATUS Halt,Interrupt;
  S=CrUsbCoreRetire(mRole,&R->Stop,&Halt,&Interrupt);
  R->HaltStatus=Halt;R->InterruptStatus=Interrupt;
  DEBUG((EFI_ERROR(S)?DEBUG_ERROR:DEBUG_INIT,"Dwc3Cr: exit role=%u halt=%r IRQ=%r result=%r\n",
    mRole,(EFI_STATUS)Halt,(EFI_STATUS)Interrupt,S));
  // This return reports software closure; distinct hardware fields retain failures.
  return EFI_SUCCESS;
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event, VOID *Context)
{
  (VOID)Event;(VOID)Context;
  CrUsbNativeExit();
}

EFI_STATUS EFIAPI Dwc3CrDxeUnload(EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  (VOID)ImageHandle;
  if (mBusy || mExiting) return EFI_ACCESS_DENIED;
  if (mTimer != NULL) gBS->SetTimer(mTimer, TimerCancel, 0);
  Status = Stop();
  if (EFI_ERROR(Status)) return Status;
  if (mFunction != NULL) {
    Status = gBS->DisconnectController(mFunction, NULL, NULL);
    if (EFI_ERROR(Status) && Status != EFI_NOT_FOUND) return Status;
    if (!CrUsbFnCanUnload()) return EFI_ACCESS_DENIED;
    Status = gBS->UninstallMultipleProtocolInterfaces(mFunction, &gEfiUsbFunctionIoProtocolGuid,
               &gCrUsbFunctionIo, &gEfiDevicePathProtocolGuid, &mFunctionPath, NULL);
    if (EFI_ERROR(Status)) return Status;
    mFunction = NULL;
  }
  if (mTimer != NULL) { gBS->CloseEvent(mTimer); mTimer = NULL; }
  if (mExit != NULL) { gBS->CloseEvent(mExit); mExit = NULL; }
  Status=CrUsbNativeRelease();
  if(Status!=EFI_SUCCESS)return Status;
  if (mPower.Initialized) return CrDwc3PowerUninit(&mPower);
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI Dwc3CrDxeEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  EDKII_IOMMU_PROTOCOL *Iommu;
  (VOID)SystemTable;
  if (mFunction != NULL || mTimer != NULL || mExit != NULL || mPower.Initialized) return EFI_ALREADY_STARTED;
  mPaths[0] = mPaths[1] = INTERCONNECT_INVALID_PATH;
#define LOCATE(Guid, Pointer) \
  Status = gBS->LocateProtocol(&(Guid), NULL, (VOID **)&(Pointer)); \
  if (EFI_ERROR(Status)) return Status
  LOCATE(gEfiUsbTypeCCrProtocolGuid, mTypeC);
  LOCATE(gEfiUsbDpPhyCrProtocolGuid, mPhy);
  LOCATE(gEfiClockCrProtocolGuid, mClock);
  LOCATE(gEfiInterconnectCrProtocolGuid, mIcb);
  LOCATE(gEfiSmmuCrProtocolGuid, mSmmu);
  LOCATE(gEdkiiIoMmuProtocolGuid, Iommu);
#undef LOCATE
  if (mTypeC == NULL || mPhy == NULL || mClock == NULL || mIcb == NULL || mSmmu == NULL || Iommu == NULL ||
      mTypeC->GetState == NULL || mPhy->Acquire == NULL || mPhy->Release == NULL ||
      mClock->Revision < EFI_CLOCK_CR_PROTOCOL_REVISION ||
      mClock->AcquireClock == NULL || mClock->AcquireGdsc == NULL ||
      mClock->SetClock == NULL || mClock->SetGdsc == NULL || mClock->SetReset == NULL ||
      mClock->SetDivider == NULL || mIcb->AcquirePath == NULL || mIcb->SetBandwidth == NULL ||
      mIcb->ReleasePath == NULL || mSmmu->Attach == NULL || Iommu->Map == NULL || Iommu->Unmap == NULL ||
      Iommu->AllocateBuffer == NULL || Iommu->FreeBuffer == NULL ||
      Iommu->Revision < EDKII_IOMMU_PROTOCOL_REVISION || Iommu->SetAttributeById == NULL) return EFI_UNSUPPORTED;
  Status = CrDwc3PowerInit(&mPower,&mPowerIo);
  if (EFI_ERROR(Status)) return Status;
  Status=CrUsbNativeInit(mSmmu,FreezeNative);
  if(Status!=EFI_SUCCESS){
    EFI_STATUS Cleanup=Dwc3CrDxeUnload(ImageHandle);
    if(Cleanup!=EFI_SUCCESS){DEBUG((DEBUG_ERROR,"USBH: setup=%r retained cleanup=%r\n",Status,Cleanup));return EFI_SUCCESS;}
    return Status;
  }
  CrUsbFnInit(Iommu);
  Status = gBS->InstallMultipleProtocolInterfaces(&mFunction, &gEfiUsbFunctionIoProtocolGuid,
             &gCrUsbFunctionIo, &gEfiDevicePathProtocolGuid, &mFunctionPath, NULL);
  if (EFI_ERROR(Status)) {
    EFI_STATUS Cleanup=Dwc3CrDxeUnload(ImageHandle);
    if(Cleanup!=EFI_SUCCESS){DEBUG((DEBUG_ERROR,"USBH: function publish=%r retained cleanup=%r\n",Status,Cleanup));return EFI_SUCCESS;}
    return Status;
  }
  mBusy = TRUE;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Exit, NULL, &gEfiEventExitBootServicesGuid, &mExit);
  if (!EFI_ERROR(Status)) Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Monitor, NULL, &mTimer);
  if (!EFI_ERROR(Status)) Status = gBS->SetTimer(mTimer, TimerPeriodic, 100000);
  mBusy = FALSE;
  if (EFI_ERROR(Status)) {
    EFI_STATUS Cleanup = Dwc3CrDxeUnload(ImageHandle);
    if (EFI_ERROR(Cleanup)) {
      DEBUG((DEBUG_ERROR, "Dwc3Cr: monitor setup: %r cleanup retained: %r\n", Status, Cleanup));
      return EFI_SUCCESS;
    }
    return Status;
  }
  DEBUG((DEBUG_INIT, "Dwc3Cr: Shell Type-C dual-role monitor ready\n"));
  return EFI_SUCCESS;
}
