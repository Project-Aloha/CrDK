/* Native host publication and lifetime. SPDX-License-Identifier: MIT */
#include "SdhciCrDxe.h"
#include "SdResources.h"
#include <Guid/EventGroup.h>
STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event,VOID *Context){
  (VOID)Event;(VOID)Context;EFI_STATUS S=CrSdHostQuiesce();
  DEBUG((S==EFI_SUCCESS?DEBUG_INFO:DEBUG_ERROR,"SdhciCr: EBS PIO host quiesce, resources retained: %r\n",S));
}
EFI_STATUS EFIAPI SdhciCrDxeEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *Table){
  (VOID)Image;(VOID)Table;if(mSdHost.Published||mSdHost.PublicationPending||mSdHost.ExitEvent)return EFI_ALREADY_STARTED;
  EFI_STATUS S=CrSdHostInit();if(S!=EFI_SUCCESS)return S;
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBoot,NULL,&gEfiEventExitBootServicesGuid,&mSdHost.ExitEvent);
  if(S!=EFI_SUCCESS)return mSdHost.ExitEvent?EFI_SUCCESS:S;
  S=CrSdResourcesAcquire(&mSdHost.Core);if(S!=EFI_SUCCESS)goto Failed;
  mSdHost.Acquired=TRUE;
  /* Fence inherited ADMA/SDMA state before any protocol/client can request IO.
   * All new transfers use the bounded PIO data port; no IOVA is manufactured. */
  S=CrSdhcReset(&mSdHost.Core,1);if(S!=EFI_SUCCESS)goto Failed;
  mSdHost.InheritedDmaStopped=TRUE;
  S=CrSdHostStopClock();if(S!=EFI_SUCCESS)goto Failed;
  S=CrSdhcEvents(&mSdHost.Core,0xffff,FALSE);if(S!=EFI_SUCCESS)goto Failed;
  mSdHost.PublicationPending=TRUE;
  S=gBS->InstallMultipleProtocolInterfaces(&mSdHost.Handle,&gEmbeddedMmcHostProtocolGuid,&mSdHost.Host,NULL);
  if(S!=EFI_SUCCESS){
    /* A non-NULL failed-publication handle may still expose a method pointer.
     * Retain the allocation/provider journal and image until EBS retirement. */
    if(mSdHost.Handle){DEBUG((DEBUG_ERROR,"SdhciCr: uncertain host publication retained: %r\n",S));return EFI_SUCCESS;}
    mSdHost.PublicationPending=FALSE;goto Failed;
  }
  mSdHost.PublicationPending=FALSE;mSdHost.Published=TRUE;
  BOOLEAN P=FALSE;EFI_STATUS Detect=CrSdResourcesCardPresent(&P);
  DEBUG((DEBUG_INFO,"SdhciCr: SDCC2 host published PIO/SDR only, card=%u detect=%r\n",P,Detect));
  S=gBS->ConnectController(mSdHost.Handle,NULL,NULL,TRUE);
  DEBUG((DEBUG_INFO,"SdhciCr: MMC class connect: %r\n",S));
  /* A published protocol is permanent through EBS even if class binding needs
   * a later BDS ConnectAll pass; never leave dangling client method pointers. */
  return EFI_SUCCESS;
Failed:;
  EFI_STATUS Cleanup=CrSdResourcesRelease(&mSdHost.Core);
  if(Cleanup==EFI_SUCCESS){
    mSdHost.Acquired=FALSE;Cleanup=gBS->CloseEvent(mSdHost.ExitEvent);
    if(Cleanup==EFI_SUCCESS)mSdHost.ExitEvent=NULL;
  }
  DEBUG((DEBUG_ERROR,"SdhciCr: startup %r cleanup %r\n",S,Cleanup));
  /* Failed cleanup keeps this image/event and exact resource journal resident. */
  return Cleanup==EFI_SUCCESS?S:EFI_SUCCESS;
}
EFI_STATUS EFIAPI SdhciCrDxeUnload(EFI_HANDLE Image){
  (VOID)Image;if(mSdHost.Published||mSdHost.PublicationPending||mSdHost.Acquired||mSdHost.ExitEvent)return EFI_ACCESS_DENIED;return EFI_SUCCESS;
}
