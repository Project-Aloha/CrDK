/** Native mapped views for the portable DWC3 register core.
    SPDX-License-Identifier: GPL-2.0-only */
#include "Dwc3.h"
#include <Library/Dwc3CrLib.h>
#include <Library/DebugLib.h>

STATIC CONST CR_DWC3_REGIONS mRegions={0x8450,
  {CR_USB_BASE+CR_DWC3_GLOBAL_OFFSET,0x1000}, {CR_USB_QSCRATCH,0x400}};
STATIC CONST CR_DWC3_WINDOW mXhci={CR_USB_BASE,0xc000};
STATIC CONST CR_DWC3_WINDOW mController={CR_USB_BASE,0x100000};
STATIC CR_DWC3_ROLE CoreRole(EFI_USB_TYPEC_DATA_ROLE Role) {
  return Role==UsbTypeCHost?CrDwc3Host:Role==UsbTypeCDevice?CrDwc3Device:0;
}
EFI_STATUS CrUsbWait(UINTN Address,UINT32 Mask,UINT32 Expected,UINT32 TimeoutUs) {
  if(Address<CR_USB_BASE || Address-CR_USB_BASE>=mController.Size)return EFI_INVALID_PARAMETER;
  UINT32 Last;
  EFI_STATUS S=CrDwc3Wait32(&mController,(UINT32)(Address-CR_USB_BASE),Mask,Expected,TimeoutUs,&Last);
  if(EFI_ERROR(S))DEBUG((DEBUG_ERROR,"Dwc3Cr: wait reg=%lx value=%08x mask=%08x expected=%08x: %r\n",Address,Last,Mask,Expected,S));
  return S;
}
EFI_STATUS CrUsbCoreHalt(EFI_USB_TYPEC_DATA_ROLE Role) {
  UINT32 Last;
  EFI_STATUS S=CrDwc3Halt(CoreRole(Role),Role==UsbTypeCHost?&mXhci:&mRegions.Global,&Last);
  if(EFI_ERROR(S))DEBUG((DEBUG_ERROR,"Dwc3Cr: halt role=%u value=%08x: %r\n",Role,Last,S));
  return S;
}
EFI_STATUS CrUsbCoreInit(EFI_USB_TYPEC_DATA_ROLE Role,BOOLEAN SuperSpeed) {
  CR_DWC3_REPORT R;
  EFI_STATUS Config=CrDwc3Configure(&mRegions,CoreRole(Role),SuperSpeed,&R);
  EFI_STATUS Halt=EFI_NOT_STARTED;
  if(!EFI_ERROR(Config))Halt=CrUsbCoreHalt(Role);
  EFI_STATUS S=EFI_ERROR(Config)?Config:Halt;
  DEBUG((EFI_ERROR(S)?DEBUG_ERROR:DEBUG_INIT,"Dwc3Cr: core id=%08x role=%u superspeed=%u config=%r halt=%r\n",R.Id,Role,SuperSpeed,Config,Halt));
  DEBUG((DEBUG_WARN,"Dwc3Cr: GHWPARAMS1=%08x interrupts=%u managed-events=%u\n",R.HwParams1,(R.HwParams1>>15)&63U,R.EventBuffers));
  DEBUG((DEBUG_WARN,"Dwc3Cr: revision=%08x type=%08x USB2-host-qualified=%u retry-disable=%u GUCTL1=%08x GUCTL3=%08x\n",R.Revision,R.VersionType,R.HostGap,R.RetryDisable,R.Guctl1,R.Guctl3));
  if(EFI_ERROR(Config))DEBUG((DEBUG_ERROR,"Dwc3Cr: config touched=%u reg=%lx value=%08x events=%u: %r\n",R.Touched,R.LastAddress,R.LastValue,R.EventBuffers,Config));
  return S;
}

EFI_STATUS CrUsbCoreRetire(EFI_USB_TYPEC_DATA_ROLE Role,CR_USB_CONTROLLER_STOP *Stop,EFI_STATUS *Halt,EFI_STATUS *Interrupt) {
  return (EFI_STATUS)CrDwc3Retire(&mRegions,&mXhci,CoreRole(Role),Stop,Halt,Interrupt);
}
