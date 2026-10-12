/* Native FocalTech AbsolutePointer adapter. MIT
 * The linked FTS protocol core is GPL-2.0-only; see its source provenance.
 * GPIO raw edge latch, deferred I2C at TPL_CALLBACK; no firmware download. */
#include <Uefi.h>
#include <Guid/EventGroup.h>
#include <Protocol/AbsolutePointer.h>
#include <Protocol/DevicePath.h>
#include <Protocol/LoadedImage.h>
#include <Protocol/EFII2cCrProtocol.h>
#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/FocalTechTouchCrLib.h>

typedef struct {
 EFI_ABSOLUTE_POINTER_PROTOCOL Pointer;
 EFI_ABSOLUTE_POINTER_MODE Mode;
 CR_FTS_TOUCH Touch;
 EFI_I2C_CR_PROTOCOL *Bus;
 EFI_GPIO_CR_PROTOCOL *Gpio;
 EFI_RPMH_CR_PROTOCOL *Rpmh;
 EFI_HANDLE Image,BusHandle,Handle;
 EFI_EVENT Poll,Exit;
 BOOLEAN BusOpened,Installed,Armed,Exiting,Busy,Cleaning,ResidentFailed,Frozen,FirstReportLogged;
 EFI_STATUS Failure;
 struct {VENDOR_DEVICE_PATH Vendor;EFI_DEVICE_PATH_PROTOCOL End;} Path;
} FTS_DXE;
STATIC FTS_DXE *mFts;
STATIC BOOLEAN mEntryBusy;
STATIC EFI_GUID mFtsPathGuid={0xf769b09b,0x8c25,0x4b28,{0xa8,0xe3,0x55,0x14,0x6a,0x12,0xc2,0x03}};
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS?S:EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Enter(FTS_DXE *D,EFI_TPL Maximum,BOOLEAN AllowExit){
 EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);EFI_STATUS S=EFI_SUCCESS;
 if(Old>Maximum||D->Busy)S=EFI_ACCESS_DENIED;
 else if(D->Exiting&&!AllowExit)S=EFI_NOT_READY;
 else D->Busy=TRUE;
 gBS->RestoreTPL(Old);return S;
}
STATIC VOID Leave(FTS_DXE *D){
 /* EBS may preempt a supplier call. After that freeze, touch no boot service
  * merely to lower the admission flag: image/context are still resident. */
 if(D->Exiting){D->Busy=FALSE;return;}
 EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);D->Busy=FALSE;gBS->RestoreTPL(Old);
}
STATIC CR_STATUS AcquirePin(VOID *C,VOID *O,CONST GpioConfigParams *P,BOOLEAN *Owned){FTS_DXE *D=C;if(D->Exiting)return EFI_ABORTED;EFI_STATUS S=Exact(D->Gpio->AcquirePin(D->Gpio,O,P,Owned));return D->Exiting?EFI_ABORTED:S;}
STATIC CR_STATUS SetReset(VOID *C,VOID *O,UINT16 Pin,UINT8 V){FTS_DXE *D=C;if(D->Frozen||(D->Exiting&&!D->Cleaning))return EFI_ABORTED;EFI_STATUS S=Exact(D->Gpio->SetPinValue(D->Gpio,O,Pin,V));return D->Frozen?EFI_ABORTED:S;}
STATIC CR_STATUS ReleasePin(VOID *C,VOID *O,UINT16 Pin){FTS_DXE *D=C;if(D->Frozen)return EFI_ABORTED;return Exact(D->Gpio->ReleasePin(D->Gpio,O,Pin));}
STATIC CR_STATUS ArmReport(VOID *C,VOID *O,UINT16 Pin){FTS_DXE *D=C;if(D->Exiting)return EFI_ABORTED;EFI_STATUS S=Exact(D->Gpio->ArmPinFallingEdge(D->Gpio,O,Pin));return D->Exiting?EFI_ABORTED:S;}
STATIC CR_STATUS TakeReport(VOID *C,VOID *O,UINT16 Pin,BOOLEAN *Pending){FTS_DXE *D=C;if(D->Exiting)return EFI_ABORTED;EFI_STATUS S=Exact(D->Gpio->TakePinEdge(D->Gpio,O,Pin,Pending));return D->Exiting?EFI_ABORTED:S;}
STATIC CR_STATUS AcquireRail(VOID *C,CONST CR_TOUCH_RAIL *R,BOOLEAN *Owned){FTS_DXE *D=C;if(D->Exiting)return EFI_ABORTED;EFI_STATUS S=Exact(D->Rpmh->AcquireVreg(D->Rpmh,R->Name,R->Millivolts,R->Mode,Owned));return D->Exiting?EFI_ABORTED:S;}
STATIC CR_STATUS ReleaseRail(VOID *C,CONST CR_TOUCH_RAIL *R){FTS_DXE *D=C;if(D->Frozen)return EFI_ABORTED;return Exact(D->Rpmh->RpmhEnableVreg(D->Rpmh,R->Name,FALSE));}
STATIC VOID DelayUs(VOID *C,UINT32 Us){FTS_DXE *D=C;if(!D->Frozen&&(!D->Exiting||D->Cleaning))MicroSecondDelay(Us);}
STATIC CR_STATUS Read(VOID *C,UINT8 Register,UINT8 *Data,UINT32 Length){
 FTS_DXE *D=C;struct {UINTN Count;EFI_I2C_OPERATION Operation[2];} P={0};
 if(D->Exiting||!D->Bus||!Data||!Length||Length>CR_FTS_REPORT_SIZE)return EFI_NOT_READY;
 UINTN Hz=100000;EFI_STATUS S=Exact(D->Bus->SetBusFrequency(D->Bus,&Hz));
 if(D->Exiting)return EFI_ABORTED;
 if(S!=EFI_SUCCESS){DEBUG((DEBUG_ERROR,"FocalTechTouchCr: I2C%u frequency before reg=%02x failed: %r\n",D->Touch.Target->I2cInstance,Register,S));return S;}
 if(Hz!=100000||D->Bus->BusFrequencyKHz!=100){DEBUG((DEBUG_ERROR,"FocalTechTouchCr: I2C%u frequency before reg=%02x mismatch Hz=%Lu bus-kHz=%u\n",D->Touch.Target->I2cInstance,Register,(UINT64)Hz,D->Bus->BusFrequencyKHz));return EFI_PROTOCOL_ERROR;}
 P.Count=2;P.Operation[0].Buffer=&Register;P.Operation[0].LengthInBytes=1;
 P.Operation[1].Buffer=Data;P.Operation[1].LengthInBytes=Length;P.Operation[1].Flags=I2C_FLAG_READ;
 S=Exact(D->Bus->StartRequest(D->Bus,D->Touch.Target->Address,(EFI_I2C_REQUEST_PACKET *)&P,NULL,NULL));
 if(!D->Exiting&&(Register==0xa3||Register==0x9f))
   DEBUG((DEBUG_WARN,"FocalTechTouchCr: I2C%u@%02x ID reg=%02x status=%r valid=%u value=%02x\n",D->Touch.Target->I2cInstance,D->Touch.Target->Address,Register,S,S==EFI_SUCCESS,S==EFI_SUCCESS?Data[0]:0));
 if(!D->Exiting&&Register==1&&Length==CR_FTS_REPORT_SIZE-1&&!D->FirstReportLogged){
   D->FirstReportLogged=TRUE;
   DEBUG((DEBUG_WARN,"FocalTechTouchCr: first report reg=%02x bytes=%u read=%r\n",Register,Length,S));
   if(S==EFI_SUCCESS)for(UINT32 I=0;I<CR_FTS_REPORT_SIZE-1;I+=8)
     DEBUG((DEBUG_WARN,"FocalTechTouchCr: report +%02u: %02x %02x %02x %02x %02x %02x %02x %02x\n",I,Data[I],Data[I+1],Data[I+2],Data[I+3],Data[I+4],Data[I+5],Data[I+6],Data[I+7]));
 }
 return D->Exiting?EFI_ABORTED:S;
}
STATIC VOID EFIAPI Wait(EFI_EVENT Event,VOID *C){FTS_DXE *D=C;if(!D->Exiting&&D->Touch.Count)gBS->SignalEvent(Event);}
STATIC VOID EFIAPI Poll(EFI_EVENT Event,VOID *C){
 FTS_DXE *D=C;(VOID)Event;if(D->Exiting||!D->Installed||!D->Armed||!D->Touch.Active)return;
 if(Enter(D,TPL_CALLBACK,FALSE)!=EFI_SUCCESS)return;
 BOOLEAN First=D->FirstReportLogged;
 EFI_STATUS S=CrFtsService(&D->Touch);
 if(!D->Exiting&&!First&&D->FirstReportLogged)
   DEBUG((DEBUG_WARN,"FocalTechTouchCr: first report validation=%r\n",S));
 if(!D->Exiting){if(S!=EFI_SUCCESS)DEBUG((DEBUG_WARN,"FocalTechTouchCr: report failed: %r\n",S));
   if(D->Touch.Count)gBS->SignalEvent(D->Pointer.WaitForInput);}
 Leave(D);
}
STATIC EFI_STATUS EFIAPI GetState(EFI_ABSOLUTE_POINTER_PROTOCOL *This,EFI_ABSOLUTE_POINTER_STATE *Out){
 if(!This||!Out||!mFts||This!=&mFts->Pointer)return EFI_INVALID_PARAMETER;FTS_DXE *D=mFts;
 if(D->Exiting)return EFI_NOT_READY;
 EFI_STATUS Admission=Enter(D,TPL_NOTIFY,FALSE);if(Admission!=EFI_SUCCESS)return Admission;
 if(D->ResidentFailed){Leave(D);return D->Failure;}
 CR_FTS_STATE S;
 EFI_STATUS Status=CrFtsGetState(&D->Touch,&S);if(Status==EFI_SUCCESS){ZeroMem(Out,sizeof(*Out));Out->CurrentX=S.X;Out->CurrentY=S.Y;Out->ActiveButtons=S.Pressed?EFI_ABSP_TouchActive:0;}
 Leave(D);return Status;
}
STATIC EFI_STATUS Disarm(FTS_DXE *D){
 if(D->Frozen)return EFI_ABORTED;
 if(!D->Armed)return EFI_SUCCESS;EFI_STATUS S=Exact(gBS->SetTimer(D->Poll,TimerCancel,0));
 if(S==EFI_SUCCESS)D->Armed=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI Reset(EFI_ABSOLUTE_POINTER_PROTOCOL *This,BOOLEAN Extended){
 if(!This||!mFts||This!=&mFts->Pointer||Extended>1)return EFI_INVALID_PARAMETER;
 FTS_DXE *D=mFts;if(D->Exiting)return EFI_NOT_READY;EFI_STATUS Admission=Enter(D,TPL_CALLBACK,FALSE);if(Admission!=EFI_SUCCESS)return Admission;
 /* Synchronous GENI requests must run below TPL_NOTIFY. Stop timer first. */
 EFI_STATUS S=Disarm(D);if(S!=EFI_SUCCESS){Leave(D);return S;}
 D->Cleaning=TRUE;S=CrFtsStop(&D->Touch);D->Cleaning=FALSE;if(S==EFI_SUCCESS&&!D->Exiting)S=CrFtsStart(&D->Touch);
 if(D->Exiting)S=EFI_ABORTED;
 if(S!=EFI_SUCCESS){CrFtsCancel(&D->Touch,S);Leave(D);return S;}
 S=Exact(gBS->SetTimer(D->Poll,TimerPeriodic,(UINT64)D->Touch.Target->PollPeriodUs*10));
 if(S==EFI_SUCCESS&&!D->Exiting)D->Armed=TRUE;Leave(D);return D->Exiting?EFI_ABORTED:S;
}
STATIC EFI_STATUS CloseEvents(FTS_DXE *D){
 EFI_STATUS S=Disarm(D);if(S!=EFI_SUCCESS)return S;
 if(D->Frozen)return EFI_ABORTED;
 if(D->Poll){S=Exact(gBS->CloseEvent(D->Poll));if(S!=EFI_SUCCESS)return S;D->Poll=NULL;if(D->Frozen)return EFI_ABORTED;}
 if(D->Exit){S=Exact(gBS->CloseEvent(D->Exit));if(S!=EFI_SUCCESS)return S;D->Exit=NULL;if(D->Frozen)return EFI_ABORTED;}
 if(D->Pointer.WaitForInput){S=Exact(gBS->CloseEvent(D->Pointer.WaitForInput));if(S!=EFI_SUCCESS)return S;D->Pointer.WaitForInput=NULL;if(D->Frozen)return EFI_ABORTED;}
 return EFI_SUCCESS;
}
STATIC EFI_STATUS Cleanup(FTS_DXE *D){
 EFI_STATUS S=Disarm(D);if(S!=EFI_SUCCESS)return S;
 if(D->Frozen)return EFI_ABORTED;
 if(D->Installed){S=Exact(gBS->UninstallMultipleProtocolInterfaces(D->Handle,&gEfiAbsolutePointerProtocolGuid,&D->Pointer,&gEfiDevicePathProtocolGuid,&D->Path,NULL));
   if(S!=EFI_SUCCESS)return S;D->Installed=FALSE;if(D->Frozen)return EFI_ABORTED;}
 S=CloseEvents(D);if(S!=EFI_SUCCESS)return S;
 if(D->Touch.Initialized){S=CrFtsStop(&D->Touch);if(S!=EFI_SUCCESS)return S;}
 if(D->BusOpened){S=Exact(gBS->CloseProtocol(D->BusHandle,&gEfiI2cCrProtocolGuid,D->Image,D->BusHandle));if(S!=EFI_SUCCESS)return S;D->BusOpened=FALSE;}
 return EFI_SUCCESS;
}
STATIC VOID EFIAPI ExitBoot(EFI_EVENT Event,VOID *C){
 FTS_DXE *D=C;(VOID)Event;
 EFI_TPL Old=gBS->RaiseTPL(TPL_HIGH_LEVEL);BOOLEAN Active=D->Busy;D->Exiting=TRUE;
 if(Active){D->Armed=FALSE;D->Frozen=TRUE;}else {D->Busy=TRUE;D->Cleaning=TRUE;}
 gBS->RestoreTPL(Old);
 /* A supplier may be midway through MMIO/I2C. Never release/reset beneath
  * that operation. The resident callback guard now rejects every new poll. */
 if(Active)return;
 EFI_STATUS S=Disarm(D);if(S==EFI_SUCCESS)S=CrFtsStop(&D->Touch);
 /* The EBS event/context stay resident through the notification. Callback
  * and timer guards prohibit future I2C even if a resource release failed. */
 if(S!=EFI_SUCCESS)DEBUG((DEBUG_ERROR,"FocalTechTouchCr: EBS retained cleanup: %r\n",S));
 D->Cleaning=FALSE;Leave(D);
}
EFI_STATUS EFIAPI FocalTechTouchCrUnload(EFI_HANDLE Image){
 if(!mFts||Image!=mFts->Image)return EFI_INVALID_PARAMETER;
 FTS_DXE *D=mFts;if(D->Exiting)return EFI_NOT_READY;EFI_STATUS S=Enter(D,TPL_CALLBACK,FALSE);if(S!=EFI_SUCCESS)return S;
 D->Cleaning=TRUE;S=Cleanup(D);D->Cleaning=FALSE;if(S!=EFI_SUCCESS){Leave(D);return S;}
 mFts=NULL;FreePool(D);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI FocalTechTouchCrEntry(EFI_HANDLE Image,EFI_SYSTEM_TABLE *System){
 (VOID)System;EFI_TPL EntryTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
 if(mFts||mEntryBusy){gBS->RestoreTPL(EntryTpl);return EFI_ALREADY_STARTED;}
 if(EntryTpl>TPL_CALLBACK){gBS->RestoreTPL(EntryTpl);return EFI_ACCESS_DENIED;}
 mEntryBusy=TRUE;gBS->RestoreTPL(EntryTpl);
 FTS_DXE *D=AllocateZeroPool(sizeof(*D));if(!D){mEntryBusy=FALSE;return EFI_OUT_OF_RESOURCES;}D->Image=Image;D->Busy=TRUE;mFts=D;
 CONST CR_TARGET_TOUCH_CONTEXT *Target=CrTargetGetTouchContext();EFI_STATUS S;EFI_LOADED_IMAGE_PROTOCOL *Loaded=NULL;
 S=Exact(gBS->HandleProtocol(Image,&gEfiLoadedImageProtocolGuid,(VOID **)&Loaded));if(S!=EFI_SUCCESS||!Loaded){if(S==EFI_SUCCESS)S=EFI_NOT_READY;goto Fail;}
 Loaded->Unload=FocalTechTouchCrUnload;
 S=Exact(gBS->LocateProtocol(&gEfiGpioCrProtocolGuid,NULL,(VOID **)&D->Gpio));if(S!=EFI_SUCCESS)goto Fail;
 S=Exact(gBS->LocateProtocol(&gEfiRpmhCrProtocolGuid,NULL,(VOID **)&D->Rpmh));if(S!=EFI_SUCCESS)goto Fail;
 if(!D->Gpio||D->Gpio->Revision<0x10003||!D->Gpio->AcquirePin||!D->Gpio->SetPinValue||!D->Gpio->ReleasePin||!D->Gpio->ArmPinFallingEdge||!D->Gpio->TakePinEdge||
    !D->Rpmh||D->Rpmh->Revision<0x10001||!D->Rpmh->AcquireVreg||!D->Rpmh->RpmhEnableVreg){S=EFI_UNSUPPORTED;goto Fail;}
 CR_FTS_IO Io={D,AcquirePin,SetReset,ReleasePin,AcquireRail,ReleaseRail,Read,DelayUs,ArmReport,TakeReport};
 S=CrFtsInit(&D->Touch,Target,&Io);if(S!=EFI_SUCCESS)goto Fail;
 EFI_HANDLE *Handles=NULL;UINTN Count=0;S=Exact(gBS->LocateHandleBuffer(ByProtocol,&gEfiI2cCrProtocolGuid,NULL,&Count,&Handles));
 if(S!=EFI_SUCCESS)goto Fail;
 for(UINTN I=0;I<Count;++I){EFI_I2C_CR_PROTOCOL *Bus=NULL;
   EFI_STATUS H=Exact(gBS->HandleProtocol(Handles[I],&gEfiI2cCrProtocolGuid,(VOID **)&Bus));
   if(H==EFI_SUCCESS&&Bus&&Bus->Revision>=EFI_I2C_CR_PROTOCOL_REVISION&&Bus->Instance==Target->I2cInstance&&Bus->StartRequest&&Bus->SetBusFrequency){
     if(D->Bus){S=EFI_COMPROMISED_DATA;break;}D->Bus=Bus;D->BusHandle=Handles[I];}}
 FreePool(Handles);if(S!=EFI_SUCCESS)goto Fail;if(!D->Bus){S=EFI_NOT_FOUND;goto Fail;}
 S=Exact(gBS->OpenProtocol(D->BusHandle,&gEfiI2cCrProtocolGuid,(VOID **)&D->Bus,Image,D->BusHandle,EFI_OPEN_PROTOCOL_BY_DRIVER));if(S!=EFI_SUCCESS)goto Fail;D->BusOpened=TRUE;
 S=CrFtsStart(&D->Touch);DEBUG((DEBUG_WARN,"FocalTechTouchCr: firmware ID qualification=%r\n",S));if(S!=EFI_SUCCESS)goto Fail;
 DEBUG((DEBUG_WARN,"FocalTechTouchCr: report IRQ armed GPIO%u; awaiting falling edge\n",Target->Interrupt.PinNumber));
 D->Mode.AbsoluteMaxX=Target->XMax;D->Mode.AbsoluteMaxY=Target->YMax;
 D->Pointer.Reset=Reset;D->Pointer.GetState=GetState;D->Pointer.Mode=&D->Mode;
 D->Path.Vendor.Header.Type=HARDWARE_DEVICE_PATH;D->Path.Vendor.Header.SubType=HW_VENDOR_DP;
 D->Path.Vendor.Header.Length[0]=sizeof(VENDOR_DEVICE_PATH);D->Path.Vendor.Guid=mFtsPathGuid;
 D->Path.End.Type=END_DEVICE_PATH_TYPE;D->Path.End.SubType=END_ENTIRE_DEVICE_PATH_SUBTYPE;D->Path.End.Length[0]=sizeof(EFI_DEVICE_PATH_PROTOCOL);
 S=Exact(gBS->CreateEvent(EVT_NOTIFY_WAIT,TPL_NOTIFY,Wait,D,&D->Pointer.WaitForInput));if(S!=EFI_SUCCESS)goto Fail;
 /* GENI StartRequest allocates descriptors; timer callbacks use TPL_CALLBACK. */
 S=Exact(gBS->CreateEvent(EVT_TIMER|EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Poll,D,&D->Poll));if(S!=EFI_SUCCESS)goto Fail;
 S=Exact(gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_NOTIFY,ExitBoot,D,&gEfiEventExitBootServicesGuid,&D->Exit));if(S!=EFI_SUCCESS)goto Fail;
 if(D->Exiting){S=EFI_ABORTED;goto Fail;}
 S=Exact(gBS->SetTimer(D->Poll,TimerPeriodic,(UINT64)Target->PollPeriodUs*10));if(S!=EFI_SUCCESS)goto Fail;
 if(D->Exiting){S=EFI_ABORTED;goto Fail;}D->Armed=TRUE;
 /* Arm before publication so no subsequent fallible step can leave an
  * installed interface pointing into a failed/unloaded image. Poll waits for
  * Installed; protocol-notify consumers already see a valid context. */
 if(D->Exiting){S=EFI_ABORTED;goto Fail;}
 S=Exact(gBS->InstallMultipleProtocolInterfaces(&D->Handle,&gEfiAbsolutePointerProtocolGuid,&D->Pointer,&gEfiDevicePathProtocolGuid,&D->Path,NULL));
 if(D->Handle)D->Installed=TRUE;if(D->Exiting){S=EFI_ABORTED;goto Fail;}if(S!=EFI_SUCCESS)goto Fail;if(!D->Installed){S=EFI_PROTOCOL_ERROR;goto Fail;}
 Loaded->Unload=FocalTechTouchCrUnload;
 mEntryBusy=FALSE;Leave(D);
 DEBUG((DEBUG_INFO,"FocalTechTouchCr: qualified firmware54/52 I2C%u@38, AbsolutePointer %ux%u IRQ polling%uus\n",Target->I2cInstance,Target->XMax,Target->YMax,Target->PollPeriodUs));return EFI_SUCCESS;
Fail:
 {
   D->Cleaning=TRUE;EFI_STATUS CleanupStatus=D->Exiting?EFI_ABORTED:Cleanup(D);D->Cleaning=FALSE;mEntryBusy=FALSE;
   if(CleanupStatus==EFI_SUCCESS){mFts=NULL;FreePool(D);}
   else {
     /* EFI_SUCCESS retains this image, not an operational pointer. Failed
      * event/protocol teardown cannot allow the loader to free live callback
      * code. Preserve exact failure for diagnostics/GetState and unload retry. */
     D->ResidentFailed=TRUE;D->Failure=S;D->Touch.Active=FALSE;D->Armed=FALSE;Leave(D);
     DEBUG((DEBUG_ERROR,"FocalTechTouchCr: RESIDENT FAILED start=%r cleanup=%r; pointer unavailable\n",S,CleanupStatus));
     return EFI_SUCCESS;
   }
 }
 return S;
}
