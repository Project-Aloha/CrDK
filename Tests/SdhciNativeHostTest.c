/* Actual native MmcHost + shared PIO register/namespace fault fixture. MIT */
#include <Uefi.h>
#include "SdTargetTestProvider.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Guid/EventGroup.h>
#include "../Driver/SdhciCrDxe/SdhciCrDxe.h"
#include "../Driver/SdhciCrDxe/SdResources.h"

EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;
EFI_GUID gEmbeddedMmcHostProtocolGuid=EMBEDDED_MMC_HOST_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
STATIC EFI_BOOT_SERVICES Boot;
STATIC UINT8 Registers[0x1000],FifoInput[512],FifoOutput[512];
STATIC UINT32 FifoPosition,FifoLength,LastIndex,Commands,Resets,FullResets,Connects,Releases,Closes,Freezes,Appends;
STATIC UINT32 ClockSettleReads,ForbiddenPowerCalls;
STATIC CR_STATUS FrozenPower(CR_SDHC *C,UINT32 Request,BOOLEAN Begin,CR_STATUS Status){(VOID)C;(VOID)Request;(VOID)Begin;(VOID)Status;++ForbiddenPowerCalls;return EFI_ACCESS_DENIED;}
STATIC BOOLEAN DelayClockEnable;
STATIC UINT32 ErrorBits,FailFifoAt;STATIC UINT64 Delays,RemoveAt;
STATIC BOOLEAN Live,Card,StuckReset,CommandTimeout,PartialPublish,PartialEvent,FailAlloc,SilentPower,ClockWithdraw;
STATIC EFI_STATUS AcquireStatus,ReleaseStatus,CreateStatus,CloseStatus,InstallStatus,FreezeStatus,ServiceStatus,ClockStatus;
STATIC EFI_TPL CurrentTpl;
STATIC EFI_EVENT_NOTIFY ExitCallback;
STATIC UINT32 RawResponse[4];

VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI AllocateZeroPool(UINTN N){return FailAlloc?NULL:calloc(1,N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
VOID EFIAPI MemoryFence(VOID){}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"assert %s:%zu %s\n",F,(size_t)L,D);abort();}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugClearMemoryEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
UINTN EFIAPI MicroSecondDelay(UINTN N){Delays+=N;if(RemoveAt&&Delays>=RemoveAt)Card=FALSE;return N;}
STATIC UINT32 R(UINT32 O,UINT32 W){UINT32 V=0;memcpy(&V,Registers+O,W);return V;}
STATIC VOID V(UINT32 O,UINT32 W,UINT32 Value){memcpy(Registers+O,&Value,W);}
STATIC UINT32 ReadMmio(UINTN A,UINT32 W){
  assert(Live);assert(A>=CR_SD_NATIVE_BASE&&A+W<=CR_SD_NATIVE_BASE+sizeof(Registers));
  UINT32 O=(UINT32)(A-CR_SD_NATIVE_BASE);
  if(O==0x20||O==0x22){
    assert(FifoPosition+W<=FifoLength);UINT32 X=0;memcpy(&X,FifoInput+FifoPosition,W);FifoPosition+=W;
    if(FifoPosition==FifoLength){V(0x30,2,R(0x30,2)|2);V(0x24,4,0);}return X;
  }
  if(O==0x2c&&ClockSettleReads){if(--ClockSettleReads==0)V(0x2c,2,7);}
  return R(O,W);
}
UINT8 EFIAPI MmioRead8(UINTN A){return (UINT8)ReadMmio(A,1);}
UINT16 EFIAPI MmioRead16(UINTN A){return (UINT16)ReadMmio(A,2);}
UINT32 EFIAPI MmioRead32(UINTN A){return ReadMmio(A,4);}
STATIC VOID WriteMmio(UINTN A,UINT32 W,UINT32 X){
  assert(Live);assert(A>=CR_SD_NATIVE_BASE&&A+W<=CR_SD_NATIVE_BASE+sizeof(Registers));
  UINT32 O=(UINT32)(A-CR_SD_NATIVE_BASE);
  if(O==0x34||O==0x38)X&=0x7fffU;
  if(O==0x20||O==0x22){assert(FifoPosition+W<=FifoLength);memcpy(FifoOutput+FifoPosition,&X,W);FifoPosition+=W;
    if(FifoPosition==FifoLength){V(0x30,2,R(0x30,2)|2);V(0x24,4,0);}return;}
  if(O==0x30||O==0x32){V(O,W,R(O,W)&~X);V(0x30,2,(R(0x30,2)&0x7fffU)|((R(0x32,2)&0x1ffU)?0x8000U:0));return;}
  if(O==0x248){V(0x240,4,R(0x240,4)&~X);return;}
  if(O==0x2f){++Resets;if(X==1){++FullResets;V(0x29,1,0);V(0x2c,2,0);}
    if(!StuckReset){V(O,1,0);V(0x24,4,0);V(0x30,2,0);V(0x32,2,0);return;}}
  if(O==0x2c&&(X&1))X|=2;
  if(O==0x2c&&(X&4)&&DelayClockEnable){X&=~4U;ClockSettleReads=3;}
  if(O==0x29)V(0x240,4,X?CR_SDHC_POWER_ON:CR_SDHC_POWER_OFF);
  V(O,W,X);
  if(O==0xe){
    ++Commands;LastIndex=X>>8;FifoPosition=0;FifoLength=X&BIT5?R(4,2):0;
    assert(!(R(0xc,2)&BIT0)); /* DMA is never selected. */
    for(UINT32 I=0;I<4;++I)V(0x10+I*4,4,RawResponse[I]);
    if(CommandTimeout)return;
    if(ErrorBits){V(0x30,2,0x8000);V(0x32,2,ErrorBits);return;}
    UINT32 Events=1;
    if(FifoLength){BOOLEAN Reading=(R(0xc,2)&BIT4)!=0;Events|=Reading?0x20:0x10;V(0x24,4,Reading?BIT11:BIT10);}
    if((X&3)==3)Events|=2;
    V(0x30,2,Events);
  }
}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 X){WriteMmio(A,1,X);return X;}
UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 X){WriteMmio(A,2,X);return X;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 X){WriteMmio(A,4,X);return X;}
EFI_STATUS CrSdResourcesAcquire(CR_SDHC *C){assert(C->Initialized);Live=TRUE;return AcquireStatus;}
EFI_STATUS CrSdResourcesReady(VOID){if(FailFifoAt&&FifoPosition>=FailFifoAt)return EFI_DEVICE_ERROR;return Live?EFI_SUCCESS:EFI_NOT_READY;}
EFI_STATUS CrSdResourcesSetClock(UINT32 Hz){assert(Hz==400000||Hz==25000000||Hz==50000000);if(ClockWithdraw)Live=ClockStatus==EFI_SUCCESS;return ClockStatus;}
EFI_STATUS CrSdResourcesCardPresent(BOOLEAN *P){*P=Card;return Live?EFI_SUCCESS:EFI_NOT_READY;}
EFI_STATUS CrSdResourcesServicePower(CR_SDHC *C){
  assert(CurrentTpl<=TPL_CALLBACK);if(ServiceStatus!=EFI_SUCCESS)return ServiceStatus;
  UINT32 Pending=0;EFI_STATUS S=CrSdhcPowerCapture(C,&Pending);
  if(S!=EFI_SUCCESS)return S;if(Pending)S=CrSdhcPowerComplete(C,Pending,TRUE,TRUE);return S;
}
EFI_STATUS CrSdResourcesRequestPower(CR_SDHC *C,BOOLEAN On){
  EFI_STATUS S=CrSdResourcesServicePower(C);if(S!=EFI_SUCCESS)return S;
  S=CrSdhcPowerRequest(C,On);if(S!=EFI_SUCCESS)return S;
  if(SilentPower)V(0x240,4,0);
  UINT32 Expected=On?CR_SDHC_POWER_ON:CR_SDHC_POWER_OFF;
  if(!(R(0x240,4)&Expected)){for(UINT32 I=0;I<100000;I+=10)MicroSecondDelay(10);return EFI_TIMEOUT;}
  return CrSdResourcesServicePower(C);
}
EFI_STATUS CrSdResourcesRelease(CR_SDHC *C){(VOID)C;++Releases;if(ReleaseStatus==EFI_SUCCESS)Live=FALSE;return ReleaseStatus;}
EFI_STATUS CrSdResourcesFreeze(CR_SDHC *C){(VOID)C;++Freezes;return FreezeStatus;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL T){assert(T>=CurrentTpl);EFI_TPL Old=CurrentTpl;CurrentTpl=T;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL T){assert(T<=CurrentTpl);CurrentTpl=T;}
STATIC EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){
  assert(Type==EVT_NOTIFY_SIGNAL&&T==TPL_NOTIFY&&C==NULL&&G==&gEfiEventExitBootServicesGuid);
  ExitCallback=N;if(CreateStatus==EFI_SUCCESS||PartialEvent)*E=(VOID *)0xe0;return CreateStatus;
}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E==(VOID *)0xe0);++Closes;return CloseStatus;}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
  assert(FullResets);va_list A;va_start(A,H);assert(va_arg(A,EFI_GUID *)==&gEmbeddedMmcHostProtocolGuid);
  assert(va_arg(A,EFI_MMC_HOST_PROTOCOL *)==&mSdHost.Host);assert(va_arg(A,VOID *)==NULL);va_end(A);++Appends;
  if(InstallStatus==EFI_SUCCESS||PartialPublish)*H=(VOID *)0xd0;return InstallStatus;
}
STATIC EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *D,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN Recursive){
  assert(H==(VOID *)0xd0&&D==NULL&&P==NULL&&Recursive);++Connects;return EFI_NOT_FOUND;
}
#include "../Driver/SdhciCrDxe/MmcHost.c"
#include "../Driver/SdhciCrDxe/SdhciCrDxe.c"
STATIC VOID Fixture(VOID){
  mTestSdTarget=NULL;mTestSdMissing=FALSE;
  ZeroMem(&mSdHost,sizeof(mSdHost));ZeroMem(&Boot,sizeof(Boot));ZeroMem(Registers,sizeof(Registers));
  Boot.RaiseTPL=Raise;Boot.RestoreTPL=Restore;Boot.CreateEventEx=Create;Boot.CloseEvent=Close;
  Boot.InstallMultipleProtocolInterfaces=Install;Boot.ConnectController=Connect;gBS=&Boot;
  AcquireStatus=ReleaseStatus=CreateStatus=CloseStatus=InstallStatus=FreezeStatus=ServiceStatus=ClockStatus=EFI_SUCCESS;
  ForbiddenPowerCalls=ClockSettleReads=0;DelayClockEnable=FALSE;
  Card=TRUE;Live=StuckReset=CommandTimeout=PartialPublish=PartialEvent=FailAlloc=SilentPower=ClockWithdraw=FALSE;
  CurrentTpl=TPL_APPLICATION;Delays=RemoveAt=0;ErrorBits=FailFifoAt=0;
  FifoPosition=FifoLength=LastIndex=Commands=Resets=FullResets=Connects=Releases=Closes=Freezes=Appends=0;
  ExitCallback=NULL;ZeroMem(RawResponse,sizeof(RawResponse));
  for(UINT32 I=0;I<512;++I)FifoInput[I]=(UINT8)(7+3*I);ZeroMem(FifoOutput,sizeof(FifoOutput));
}
STATIC EFI_MMC_HOST_PROTOCOL *Start(VOID){
  assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS);assert(mSdHost.Published&&Connects==1&&FullResets==1);
  assert(mSdHost.Host.NotifyState(&mSdHost.Host,MmcHwInitializationState)==EFI_SUCCESS);
  assert(mSdHost.Initialized&&mSdHost.Clock==400000);assert(R(0x20c,4)&BIT15);return &mSdHost.Host;
}
int main(void){
  Fixture();mTestSdMissing=TRUE;
  assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_UNSUPPORTED&&!mSdHost.ExitEvent&&!mSdHost.Acquired&&!Commands&&!Resets);
  Fixture();CR_TARGET_SD_CONTEXT Bad=*CrTargetGetSdContext();Bad.Paths[1].Peak=0;mTestSdTarget=&Bad;
  assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_COMPROMISED_DATA&&!mSdHost.ExitEvent&&!mSdHost.Acquired&&!Commands&&!Resets);

  Fixture();Card=FALSE;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS);assert(Commands==0&&Delays==0);
  assert(!mSdHost.Host.IsCardPresent(&mSdHost.Host));assert(SdhciCrDxeUnload(NULL)==EFI_ACCESS_DENIED);
  Fixture();EFI_MMC_HOST_PROTOCOL *H=Start();assert(!H->IsMultiBlock(H));
  UINT32 Reply[4];EFI_DEVICE_PATH_PROTOCOL *Path=NULL;assert(H->BuildDevicePath(H,&Path)==EFI_SUCCESS);assert(Path->Type==HARDWARE_DEVICE_PATH);FreePool(Path);
  FailAlloc=TRUE;assert(H->BuildDevicePath(H,&Path)==EFI_OUT_OF_RESOURCES&&Path==NULL);FailAlloc=FALSE;
  assert(H->SendCommand(H,MMC_CMD1,0)==EFI_UNSUPPORTED);assert(H->SendCommand(H,MMC_CMD5,0)==EFI_UNSUPPORTED);
  assert(H->SendCommand(H,MMC_CMD11,0)==EFI_UNSUPPORTED);assert(H->SetIos(H,100000000,4,0)==EFI_UNSUPPORTED);
  assert(H->SetIos(H,25000000,1,0)==EFI_SUCCESS);assert(H->SetIos(H,50000000,4,0)==EFI_SUCCESS);assert(R(0x28,1)&BIT1);
  DelayClockEnable=TRUE;assert(H->SetIos(H,25000000,4,0)==EFI_SUCCESS&&!ClockSettleReads);
  DelayClockEnable=FALSE;
  ClockWithdraw=TRUE;ClockStatus=EFI_DEVICE_ERROR;assert(H->SetIos(H,25000000,4,0)==EFI_DEVICE_ERROR&&!Live&&mSdHost.CardClockStopped);
  ClockStatus=EFI_SUCCESS;assert(H->SetIos(H,25000000,4,0)==EFI_SUCCESS&&Live&&!mSdHost.CardClockStopped);ClockWithdraw=FALSE;
  RawResponse[0]=0x12345678;RawResponse[1]=0x90abcdef;RawResponse[2]=0xa1b2c3d4;RawResponse[3]=0x1234567;
  assert(H->SendCommand(H,MMC_CMD9,0)==EFI_SUCCESS);assert(H->ReceiveResponse(H,MMC_RESPONSE_TYPE_R2,Reply)==EFI_SUCCESS);
  assert(Reply[0]==0x34567800&&Reply[1]==0xabcdef12&&Reply[2]==0xb2c3d490&&Reply[3]==0x234567a1);
  assert(H->ReceiveResponse(H,MMC_RESPONSE_TYPE_R1,Reply)==EFI_NOT_READY);
  RawResponse[0]=0xffff0100;assert(H->SendCommand(H,MMC_CMD3,0)==EFI_SUCCESS);
  assert(H->ReceiveResponse(H,MMC_RESPONSE_TYPE_R6,Reply)==EFI_SUCCESS&&Reply[0]==0xffff0100);
  RawResponse[0]=0x12348000;assert(H->SendCommand(H,MMC_CMD3,0)==EFI_CRC_ERROR);
  RawResponse[0]=0x12344000;assert(H->SendCommand(H,MMC_CMD3,0)==EFI_UNSUPPORTED);
  RawResponse[0]=0x12342000;assert(H->SendCommand(H,MMC_CMD3,0)==EFI_DEVICE_ERROR);
  RawResponse[0]=0x100;assert(H->SendCommand(H,MMC_CMD3,0)==EFI_DEVICE_ERROR);
  RawResponse[0]=MMC_STATUS_APP_CMD;assert(H->SendCommand(H,MMC_CMD55,0)==EFI_SUCCESS);
  RawResponse[0]=0xc0ff8000;assert(H->SendCommand(H,MMC_ACMD41,0)==EFI_SUCCESS);
  RawResponse[0]=0x1ce;assert(H->SendCommand(H,MMC_CMD8,0x1ce)==EFI_SUCCESS);
  RawResponse[0]=BIT22;assert(H->SendCommand(H,MMC_CMD17,0)==EFI_UNSUPPORTED&&!mSdHost.DataPending&&!mSdHost.ResponseValid);
  RawResponse[0]=BIT26;assert(H->SendCommand(H,MMC_CMD24,0)==EFI_WRITE_PROTECTED&&!mSdHost.DataPending);
  RawResponse[0]=BIT23;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_CRC_ERROR);
  RawResponse[0]=BIT19;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_DEVICE_ERROR);
  RawResponse[0]=0;
  UINT8 Unaligned[515];memset(Unaligned,0xa5,sizeof(Unaligned));
  assert(H->SendCommand(H,MMC_CMD17,42)==EFI_SUCCESS);assert(LastIndex==17&&R(8,4)==42);
  assert(H->SendCommand(H,MMC_CMD13,0)==EFI_NOT_READY);
  assert(H->ReadBlockData(H,42,511,(UINT32 *)(VOID *)(Unaligned+1))==EFI_BAD_BUFFER_SIZE);
  assert(H->WriteBlockData(H,42,512,(UINT32 *)(VOID *)(Unaligned+1))==EFI_BAD_BUFFER_SIZE);
  assert(H->ReadBlockData(H,42,512,(UINT32 *)(VOID *)(Unaligned+1))==EFI_SUCCESS);
  assert(!memcmp(Unaligned+1,FifoInput,512)&&Unaligned[0]==0xa5&&Unaligned[513]==0xa5&&!mSdHost.Core.DataActive);
  assert(H->SendCommand(H,MMC_CMD24,43)==EFI_SUCCESS);
  assert(H->WriteBlockData(H,43,512,(UINT32 *)(VOID *)(Unaligned+1))==EFI_SUCCESS);assert(!memcmp(FifoOutput,Unaligned+1,512));
  RawResponse[0]=MMC_STATUS_APP_CMD;assert(H->SendCommand(H,MMC_CMD55,0)==EFI_SUCCESS);
  assert(H->SendCommand(H,MMC_CMD11,0)==EFI_UNSUPPORTED&&mSdHost.Application);
  assert(H->SendCommand(H,MMC_INDX(64),0)==EFI_INVALID_PARAMETER&&mSdHost.Application);
  assert(H->SendCommand(H,MMC_CMD6,2)==EFI_SUCCESS);assert(!mSdHost.DataPending&&FifoLength==0);
  assert(H->SendCommand(H,MMC_CMD6,0x00fffff0)==EFI_SUCCESS);assert(mSdHost.DataPending&&FifoLength==64);
  assert(H->ReadBlockData(H,0,64,(UINT32 *)(VOID *)(Unaligned+1))==EFI_SUCCESS);
  assert(H->SendCommand(H,MMC_CMD55,0)==EFI_SUCCESS);assert(H->SendCommand(H,MMC_ACMD51,0)==EFI_SUCCESS);
  assert(H->ReadBlockData(H,0,8,(UINT32 *)(VOID *)(Unaligned+1))==EFI_SUCCESS);
  memset(Unaligned,0xa5,sizeof(Unaligned));assert(H->SendCommand(H,MMC_CMD17,44)==EFI_SUCCESS);FailFifoAt=16;
  assert(H->ReadBlockData(H,44,512,(UINT32 *)(VOID *)(Unaligned+1))==EFI_DEVICE_ERROR);
  assert(mSdHost.Core.NeedsReset&&mSdHost.Core.Transferred==16&&Unaligned[1]==0xa5);FailFifoAt=0;
  assert(H->SendCommand(H,MMC_CMD13,0)==EFI_NOT_READY&&!mSdHost.Core.NeedsReset);
  ErrorBits=BIT1;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_DEVICE_ERROR);assert(!mSdHost.Core.NeedsReset&&!mSdHost.DataPending);
  ErrorBits=0;CommandTimeout=TRUE;UINT64 Before=Delays;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_TIMEOUT);
  assert(Delays-Before==1000000&&CurrentTpl==TPL_APPLICATION);CommandTimeout=FALSE;
  RemoveAt=Delays+30;CommandTimeout=TRUE;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_NO_MEDIA);CommandTimeout=FALSE;Card=TRUE;RemoveAt=0;
  ServiceStatus=EFI_DEVICE_ERROR;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_DEVICE_ERROR);ServiceStatus=EFI_SUCCESS;
  assert(mSdHost.InheritedDmaStopped);mSdHost.Core.SyncPower=FrozenPower;
  UINT32 PriorFullResets=FullResets;
  assert(CrSdHostQuiesce()==EFI_SUCCESS&&mSdHost.Exiting&&Freezes==1&&!mSdHost.Initialized);
  assert(!ForbiddenPowerCalls&&FullResets==PriorFullResets&&R(0x29,1)==0x0d);
  UINT32 OldCommands=Commands;assert(H->SendCommand(H,MMC_CMD13,0)==EFI_NOT_READY&&Commands==OldCommands);assert(!H->IsCardPresent(H));
  Fixture();AcquireStatus=EFI_DEVICE_ERROR;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&Releases==1&&Closes==1&&!mSdHost.ExitEvent);
  Fixture();AcquireStatus=EFI_DEVICE_ERROR;CloseStatus=EFI_DEVICE_ERROR;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&mSdHost.ExitEvent&&SdhciCrDxeUnload(NULL)==EFI_ACCESS_DENIED);
  Fixture();AcquireStatus=EFI_DEVICE_ERROR;ReleaseStatus=EFI_DEVICE_ERROR;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Live&&Closes==0&&mSdHost.ExitEvent);
  Fixture();InstallStatus=EFI_DEVICE_ERROR;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&!Live&&Closes==1);
  Fixture();InstallStatus=EFI_DEVICE_ERROR;PartialPublish=TRUE;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Live&&mSdHost.PublicationPending&&Releases==0);
  assert(SdhciCrDxeUnload(NULL)==EFI_ACCESS_DENIED&&SdhciCrDxeEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
  Fixture();CreateStatus=EFI_DEVICE_ERROR;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&!Live&&Releases==0);
  Fixture();CreateStatus=EFI_DEVICE_ERROR;PartialEvent=TRUE;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&mSdHost.ExitEvent&&SdhciCrDxeUnload(NULL)==EFI_ACCESS_DENIED);
  Fixture();H=Start();mSdHost.InheritedDmaStopped=FALSE;mSdHost.Core.SyncPower=FrozenPower;
  assert(CrSdHostQuiesce()==EFI_ACCESS_DENIED&&ForbiddenPowerCalls==1&&mSdHost.Acquired&&Live);
  Fixture();H=Start();StuckReset=TRUE;assert(CrSdHostQuiesce()==EFI_TIMEOUT&&mSdHost.Exiting&&mSdHost.Acquired&&Live);
  assert(H->ReadBlockData(H,0,512,Reply)==EFI_NOT_READY);
  Fixture();H=Start();FreezeStatus=EFI_DEVICE_ERROR;UINT32 BeforeReset=Resets;assert(CrSdHostQuiesce()==EFI_DEVICE_ERROR&&Resets==BeforeReset&&Live);
  assert(H->SendCommand(H,MMC_CMD13,0)==EFI_NOT_READY);
  Fixture();H=Start();mSdHost.Busy=TRUE;assert(CrSdHostQuiesce()==EFI_NOT_READY&&Live&&mSdHost.Exiting);
  assert(CrSdhcRead(&mSdHost.Core,0x24,4,&Reply[0])==EFI_NOT_READY);
  Fixture();H=Start();assert(ExitCallback);CurrentTpl=TPL_NOTIFY;ExitCallback(mSdHost.ExitEvent,NULL);CurrentTpl=TPL_APPLICATION;
  assert(mSdHost.Exiting&&!mSdHost.Initialized&&mSdHost.Acquired&&Live&&Freezes==1);
  assert(H->SendCommand(H,MMC_CMD13,0)==EFI_NOT_READY);
  Fixture();assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS);SilentPower=TRUE;
  assert(mSdHost.Host.NotifyState(&mSdHost.Host,MmcHwInitializationState)==EFI_TIMEOUT&&!mSdHost.Initialized);
  Fixture();H=Start();ClockWithdraw=TRUE;ClockStatus=EFI_DEVICE_ERROR;assert(H->SetIos(H,25000000,4,0)==EFI_DEVICE_ERROR&&!Live);
  ClockStatus=EFI_SUCCESS;assert(H->NotifyState(H,MmcHwInitializationState)==EFI_SUCCESS&&Live&&mSdHost.Initialized&&mSdHost.Clock==400000);
  puts("SD native actual host/core: SD response layout, SCR/switch/unaligned512 PIO, bounded timeout/removal/reset, no-card start, namespace/event retained failures and EBS quiesce PASS");
  return 0;
}
