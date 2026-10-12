/* Inactive HDK final UART writer handoff. SPDX-License-Identifier: MIT */
#include <PiDxe.h>
#include <Library/UartHandoffUefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Guid/EventGroup.h>
#ifndef CR_UART_COVERAGE_HEADER
#define CR_UART_COVERAGE_HEADER "UartFirmwareCoverage.h"
#endif
#include CR_UART_COVERAGE_HEADER

STATIC CR_UART_UEFI_HANDOFF mHandoff;
STATIC EFI_HANDLE mImage;
STATIC EFI_SYSTEM_TABLE *mSystem;
STATIC EFI_EVENT mReady, mExit;
STATIC BOOLEAN mEntered, mUnloading, mBusy, mTerminal;

/* A live read prevents the compiler/linker from substituting a stale or
 * discarded build stamp. This is build correspondence, not authentication or
 * a retirement witness. Count0 remains invalid in the real Prepare path. */
STATIC BOOLEAN CoverageStampValid(VOID){
  STATIC CONST UINT8 Magic[16]={'C','R','U','A','R','T','B','U','I','L','D','V','1',0,0,0};
  CONST UINT8 *Payload=(CONST UINT8 *)&mUartFirmwareCoverage;
  UINT32 Crc=0xffffffffU;
  if(sizeof(mUartFirmwareCoverage)!=1064 || mUartFirmwareCoverage.Revision!=1 ||
     mUartFirmwareCoverage.Count>CR_UART_HANDOFF_IMAGES)return FALSE;
  for(UINTN I=0;I<16;I++)if(mUartCoverageBuildStamp[I]!=Magic[I])return FALSE;
  for(UINTN I=0;I<40;I++)if(mUartCoverageBuildStamp[16+I]!=Payload[I])return FALSE;
  for(UINTN I=0;I<sizeof(mUartFirmwareCoverage);I++){
    Crc^=Payload[I];for(UINTN Bit=0;Bit<8;Bit++)Crc=(Crc>>1)^((0U-(Crc&1U))&0xedb88320U);
  }
  Crc^=0xffffffffU;
  for(UINTN I=0;I<4;I++)if(mUartCoverageBuildStamp[56+I]!=(UINT8)(Crc>>(8*I)) ||
                           mUartCoverageBuildStamp[60+I]!=0)return FALSE;
  return TRUE;
}

STATIC VOID EFIAPI Ready(EFI_EVENT Event,VOID *Context){
  (VOID)Event;(VOID)Context;
  if(mUnloading || mTerminal)return; /* no boot-service pointer after closure */
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);
  if(mUnloading || mBusy || mTerminal){gBS->RestoreTPL(Old);return;}
  mBusy=TRUE;gBS->RestoreTPL(Old);
  CrUartHandoffUefiPrepare(&mHandoff,mImage,mSystem,&mUartFirmwareCoverage);
  Old=gBS->RaiseTPL(TPL_NOTIFY);mBusy=FALSE;gBS->RestoreTPL(Old);
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event,VOID *Context){
  (VOID)Event;(VOID)Context;
  /* Core signals this group only after the final MapKey succeeds. Do not
   * register with BeforeExitBootServices: a failed attempt must keep logging. */
  if(mTerminal)return;
  mTerminal=TRUE;
  if(mUnloading || mBusy)return;
  CrUartHandoffUefiFinalize(&mHandoff);
  /* All state/pages/code and events now remain until reboot. Runtime library
   * callbacks also close their private gates, independently of event order. */
}
EFI_STATUS EFIAPI CrUartHandoffUnload(EFI_HANDLE Image){
  if(!mEntered || Image!=mImage)return EFI_INVALID_PARAMETER;
  if(mTerminal)return EFI_ACCESS_DENIED; /* no boot pointer after actual EBS */
  EFI_TPL Old=gBS->RaiseTPL(TPL_NOTIFY);
  if(mBusy){gBS->RestoreTPL(Old);return EFI_NOT_READY;}
  mUnloading=TRUE;
  /* Valid owned CoreCloseEvent at this TPL removes any queued notification.
   * These exact events are never published or closed by another owner. */
  EFI_STATUS R=EFI_SUCCESS;
  if(mReady){R=gBS->CloseEvent(mReady);if(R==EFI_SUCCESS)mReady=NULL;}
  if(R==EFI_SUCCESS && mExit){R=gBS->CloseEvent(mExit);if(R==EFI_SUCCESS)mExit=NULL;}
  gBS->RestoreTPL(Old);
  if(R!=EFI_SUCCESS)return EFI_ERROR(R)?R:EFI_NOT_READY;
  R=CrUartHandoffUefiRelease(&mHandoff);
  if(R==EFI_SUCCESS)mEntered=FALSE;
  return R;
}
EFI_STATUS EFIAPI CrUartHandoffEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System){
  if(!Image || !System || System->BootServices!=gBS)return EFI_INVALID_PARAMETER;
  if(mEntered)return EFI_ALREADY_STARTED;
  if(!CoverageStampValid())return EFI_COMPROMISED_DATA;
  mImage=Image;mSystem=System;mEntered=TRUE;
  EFI_STATUS R=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,
      &gEfiEventExitBootServicesGuid,&mExit);
  if(R==EFI_SUCCESS)R=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Ready,NULL,
      &gEfiEventReadyToBootGuid,&mReady);
  if(R!=EFI_SUCCESS){
    EFI_STATUS Failure=EFI_ERROR(R)?R:EFI_NOT_READY;
    /* If cleanup fails, keep this image resident with closed callback gates
     * and exact event/page ownership for an explicit Unload retry. */
    R=CrUartHandoffUnload(Image);return R==EFI_SUCCESS?Failure:EFI_SUCCESS;
  }
  Ready(NULL,NULL); /* ACPI/monotonic may arrive later at real ReadyToBoot. */
  return EFI_SUCCESS;
}
