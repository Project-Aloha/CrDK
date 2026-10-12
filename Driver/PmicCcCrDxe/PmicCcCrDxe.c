/* Native adapter for the direct PM8150B CC observer.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Protocol/EFICrSpmiProtocol.h>
#include <Protocol/EFIUsbTypeCCrProtocol.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
STATIC EFI_CR_SPMI_PROTOCOL *mProvider;
STATIC CR_SPMI_CLIENT mClient;
STATIC CR_SPMI_BUS mBus;
STATIC CR_PMIC_CC mPort;
STATIC EFI_USB_TYPEC_CR_PROTOCOL mProtocol;
STATIC EFI_HANDLE mHandle;
STATIC EFI_EVENT mWork,mTimer,mExit;
STATIC BOOLEAN mStopped,mQueued,mRunning,mPublished;
STATIC BOOLEAN mEntered,mEntryBusy,mExiting,mStopping;
STATIC UINT8 mRetry;
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC VOID Wake(VOID *X){
 (VOID)X;if(mStopped||mExiting||mQueued||!mWork)return;mQueued=TRUE;mRetry=0;
 EFI_STATUS S=Exact(gBS->SignalEvent(mWork));
 if(S!=EFI_SUCCESS)CrPmicCcInvalidate(&mPort,S);
}
STATIC VOID EFIAPI Service(EFI_EVENT E,VOID *X){
 (VOID)X;CR_INTERRUPT_GUARD G;
 if(E==mWork)mQueued=FALSE;
 if(mStopped||mExiting||mRunning)return;
 EFI_STATUS S=Exact(CrInterruptGuardAcquire(&G));
 if(S!=EFI_SUCCESS){CrPmicCcInvalidate(&mPort,S);return;}
 if(mStopped||mExiting){CrInterruptGuardRelease(&G);return;}
 mRunning=TRUE;S=CrPmicCcService(&mPort);
 BOOLEAN Retry=!mStopped&&!mExiting&&S!=EFI_SUCCESS && mRetry<3 && mBus.Check(&mBus)==EFI_SUCCESS;
 if(Retry)mRetry++;
 if(S==EFI_SUCCESS)DEBUG((DEBUG_WARN,"PmicTypeC: CC seq=%u attached=%u source=%u orientation=%u data=%u (PD policy unavailable)\n",
  mPort.State.Sequence,mPort.State.Connected,mPort.State.PowerSource,mPort.State.Orientation,mPort.State.DataRole));
 CrInterruptGuardRelease(&G);
 if(mStopped||mExiting){mRunning=FALSE;return;}
 /* Bounded retry only for an IRQ/setup snapshot; idle does not poll the PMIC. */
 S=Exact(gBS->SetTimer(mTimer,Retry?TimerRelative:TimerCancel,Retry?10000:0));
 if(S!=EFI_SUCCESS)CrPmicCcInvalidate(&mPort,S);mRunning=FALSE;
}
STATIC EFI_STATUS EFIAPI Get(EFI_USB_TYPEC_CR_PROTOCOL *This,EFI_USB_TYPEC_STATE *Out){
 if(This!=&mProtocol||!Out)return EFI_INVALID_PARAMETER;
 if(mStopped||mExiting)return EFI_NOT_READY;
 CR_INTERRUPT_GUARD G;EFI_STATUS S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS)return S;
 S=mStopped||mExiting?EFI_NOT_READY:CrPmicCcGet(&mPort,Out);
 CrInterruptGuardRelease(&G);return S;
}
STATIC EFI_STATUS EFIAPI Data(EFI_USB_TYPEC_CR_PROTOCOL *This,EFI_USB_TYPEC_DATA_ROLE Role){
 (VOID)Role;return This==&mProtocol?EFI_UNSUPPORTED:EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS EFIAPI Witness(EFI_USB_TYPEC_CR_PROTOCOL *This,CR_TYPEC_NON_PD_HOST_WITNESS *Out){
 if(This!=&mProtocol||!Out)return EFI_INVALID_PARAMETER;
 /* Pure atomic publication plus the retained provider's RAM-only Check.
  * SMMU stopped verification must not invoke TPL/BS or SPMI here. */
 return mStopped||mExiting?EFI_NOT_READY:CrPmicCcHostWitness(&mPort,Out);
}
STATIC EFI_STATUS EFIAPI Power(EFI_USB_TYPEC_CR_PROTOCOL *This,BOOLEAN Source){
 (VOID)Source;return This==&mProtocol?EFI_UNSUPPORTED:EFI_INVALID_PARAMETER;
}
STATIC EFI_STATUS Stop(VOID){
 mStopped=TRUE;CrPmicCcClose(&mPort);
 if(mExiting)return EFI_ACCESS_DENIED;
 if(mEntryBusy||mRunning||mStopping)return EFI_NOT_READY;
 mStopping=TRUE;CR_INTERRUPT_GUARD G;
 EFI_STATUS S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS)goto Done;
 if(mExiting){S=EFI_ACCESS_DENIED;CrInterruptGuardRelease(&G);goto Done;}
 S=CrPmicCcStop(&mPort);
 if(mExiting){S=EFI_ACCESS_DENIED;CrInterruptGuardRelease(&G);goto Done;}
 if(S==EFI_SUCCESS && mClient.Provider)S=mProvider->Release(mProvider,&mClient);
 CrInterruptGuardRelease(&G);
 if(mExiting){S=EFI_ACCESS_DENIED;goto Done;}if(S!=EFI_SUCCESS)goto Done;
 if(mTimer){S=Exact(gBS->SetTimer(mTimer,TimerCancel,0));if(mExiting){S=EFI_ACCESS_DENIED;goto Done;}if(S!=EFI_SUCCESS)goto Done;}
 if(mQueued||mRunning){S=EFI_NOT_READY;goto Done;}
 EFI_EVENT *Events[]={&mWork,&mTimer,&mExit};for(UINTN I=0;I<3;I++)if(*Events[I]){
  S=Exact(gBS->CloseEvent(*Events[I]));if(S==EFI_SUCCESS)*Events[I]=NULL;
  if(mExiting){S=EFI_ACCESS_DENIED;goto Done;}if(S!=EFI_SUCCESS)goto Done;}
Done:
 mStopping=FALSE;return S;
}
STATIC VOID EFIAPI Exit(EFI_EVENT E,VOID *X){
 (VOID)E;(VOID)X;mExiting=mStopped=TRUE;CrPmicCcClose(&mPort);
 /* EBS closes cached admission only. Exact client/IRQ/event claims and the
  * producer image remain resident; provider revocation owns hardware drain. */
}
EFI_STATUS EFIAPI PmicCcCrUnload(EFI_HANDLE Image){
 if(mEntryBusy||mStopping||mExiting)return EFI_ACCESS_DENIED;
 (VOID)Image;EFI_STATUS S=Stop();if(S!=EFI_SUCCESS)return S;
 if(mPublished){S=Exact(gBS->UninstallMultipleProtocolInterfaces(mHandle,&gEfiUsbTypeCCrProtocolGuid,&mProtocol,NULL));if(S==EFI_SUCCESS)mPublished=FALSE;}return S;
}
STATIC EFI_STATUS Entry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System){
 (VOID)Image;(VOID)System;EFI_STATUS S,Cleanup;CR_PMIC_CC_TARGET Target;CR_INTERRUPT_GUARD G;
 if(mPublished||mClient.Provider||mPort.Initialized)return EFI_ALREADY_STARTED;
 S=Exact(gBS->LocateProtocol(&gEfiCrSpmiProtocolGuid,NULL,(VOID **)&mProvider));if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)return S;
 if(!mProvider||mProvider->Revision!=EFI_CR_SPMI_PROTOCOL_REVISION||mProvider->Size<sizeof(*mProvider))return EFI_UNSUPPORTED;
 S=Exact(mProvider->GetTypeCTarget(mProvider,&Target));if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)return S;
 EFI_USB_TYPEC_CR_PROTOCOL *Existing=NULL;
 S=Exact(gBS->LocateProtocol(&gEfiUsbTypeCCrProtocolGuid,NULL,(VOID **)&Existing));if(mExiting)goto ExitHeld;if(S==EFI_SUCCESS)return EFI_ALREADY_STARTED;
 if(S!=EFI_NOT_FOUND)return S;
 mStopped=FALSE;
 S=Exact(gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Service,NULL,&mWork));if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)goto Failure;
 S=Exact(gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Service,NULL,&mTimer));if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)goto Failure;
 S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,Exit,NULL,&gEfiEventExitBootServicesGuid,&mExit));if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)goto Failure;
 S=Exact(CrInterruptGuardAcquire(&G));if(S!=EFI_SUCCESS)goto Failure;
 if(!mExiting)S=Exact(mProvider->Acquire(mProvider,CrSpmiClientTypeC,&mPort,&mClient,&mBus));
 if(!mExiting&&S==EFI_SUCCESS)S=CrPmicCcStart(&mPort,&Target,&mBus,Wake,NULL);
 CrInterruptGuardRelease(&G);if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)goto Failure;
 mProtocol=(EFI_USB_TYPEC_CR_PROTOCOL){EFI_USB_TYPEC_NON_PD_WITNESS_REVISION,Get,Data,Power,Witness};
 S=Exact(gBS->InstallMultipleProtocolInterfaces(&mHandle,&gEfiUsbTypeCCrProtocolGuid,&mProtocol,NULL));if(S==EFI_SUCCESS)mPublished=TRUE;
 if(mExiting)goto ExitHeld;if(S!=EFI_SUCCESS)goto Failure;
 DEBUG((DEBUG_WARN,"PmicTypeC: HDK%u PM8150B rev%u CC IRQ observer ready; batteryless=%u\n",Target.Board,mPort.Revision[3],Target.Batteryless));return EFI_SUCCESS;
Failure:
 /* Entry has finished using the provider before normal partial retirement. */
 mEntryBusy=FALSE;
 Cleanup=Stop();DEBUG((DEBUG_ERROR,"PmicTypeC: start=%r retained cleanup=%r\n",S,Cleanup));return Cleanup==EFI_SUCCESS?S:EFI_SUCCESS;
ExitHeld:
 return EFI_SUCCESS;
}
EFI_STATUS EFIAPI PmicCcCrEntryPoint(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System){
 if(mEntered||mEntryBusy||mExiting)return EFI_ALREADY_STARTED;
 mEntered=mEntryBusy=TRUE;EFI_STATUS S=Entry(Image,System);mEntryBusy=FALSE;return S;
}
