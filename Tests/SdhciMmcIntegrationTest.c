/* Real class/host/core end-to-end initialization with independent SD card.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Mmc.h"
#include "SdTargetTestProvider.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>
#include <Guid/EventGroup.h>
#include "../Driver/SdhciCrDxe/SdhciCrDxe.h"
#include "../Driver/SdhciCrDxe/SdResources.h"
EFI_BOOT_SERVICES *gBS;EFI_SYSTEM_TABLE *gST;STATIC EFI_BOOT_SERVICES Boot;
EFI_GUID gEmbeddedMmcHostProtocolGuid=EMBEDDED_MMC_HOST_PROTOCOL_GUID;
EFI_GUID gEfiEventExitBootServicesGuid=EFI_EVENT_GROUP_EXIT_BOOT_SERVICES;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid,gEfiDriverDiagnostics2ProtocolGuid;
EFI_COMPONENT_NAME_PROTOCOL gMmcComponentName;EFI_COMPONENT_NAME2_PROTOCOL gMmcComponentName2;
EFI_DRIVER_DIAGNOSTICS2_PROTOCOL gMmcDriverDiagnostics2;
STATIC UINT8 Registers[0x1000],Fifo[512],Storage[8][512];
STATIC UINT32 Position,Length,Index,Argument,Commands,DataCommands,FailDataCommand,FullResets,PowerServices,ClockRate;
STATIC UINTN Pools;STATIC EFI_TPL Tpl;STATIC BOOLEAN Card,Live,Opened,Application,SwitchAccepted,CardHighSpeed,RejectWidth;
STATIC UINT32 RemoveCommand,FailOcrCommand,OcrBusyReplies,OcrCalls;
STATIC EFI_STATUS ReinstallStatus;STATIC BOOLEAN ResetDuringReinstall;
STATIC EFI_BLOCK_IO_PROTOCOL *Block;STATIC EFI_EVENT_NOTIFY ExitCallback;
STATIC EFI_HANDLE HostHandle=(VOID *)0xd0,BlockHandle=(VOID *)0xe0;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID *EFIAPI ZeroMem(VOID *D,UINTN N){return memset(D,0,N);}
VOID *EFIAPI AllocatePool(UINTN N){VOID *P=malloc(N);if(P)++Pools;return P;}
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=AllocatePool(N);if(P)memset(P,0,N);return P;}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *S){VOID *P=AllocatePool(N);if(P)memcpy(P,S,N);return P;}
VOID EFIAPI FreePool(VOID *P){assert(P&&Pools);--Pools;free(P);}
VOID *EFIAPI AllocatePages(UINTN N){return AllocateZeroPool(N*4096);}
VOID EFIAPI FreePages(VOID *P,UINTN N){FreePool(P);}
UINT64 EFIAPI MultU64x32(UINT64 V,UINT32 M){return V*M;}
VOID EFIAPI MemoryFence(VOID){}
UINTN EFIAPI MicroSecondDelay(UINTN N){return N;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){}
UINTN EFIAPI Print(CONST CHAR16 *F,...){return 0;}
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *H){H->ForwardLink=H->BackLink=H;return H;}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *H,LIST_ENTRY *P){P->BackLink=H->BackLink;P->ForwardLink=H;H->BackLink->ForwardLink=P;H->BackLink=P;return H;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *P){P->BackLink->ForwardLink=P->ForwardLink;P->ForwardLink->BackLink=P->BackLink;return P->ForwardLink;}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *P){return ((CONST EFI_DEVICE_PATH_PROTOCOL *)P)->Type==END_DEVICE_PATH_TYPE;}
VOID EFIAPI SetDevicePathEndNode(VOID *P){*(EFI_DEVICE_PATH_PROTOCOL *)P=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};}
UINTN EFIAPI DevicePathNodeLength(CONST VOID *P){CONST EFI_DEVICE_PATH_PROTOCOL *D=P;return D->Length[0]|(D->Length[1]<<8);}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(CONST EFI_DEVICE_PATH_PROTOCOL *D,CONST EFI_DEVICE_PATH_PROTOCOL *N){return AllocateCopyPool(DevicePathNodeLength(N),N);}
EFI_STATUS EFIAPI EfiLibInstallDriverBindingComponentName2(EFI_HANDLE H,CONST EFI_SYSTEM_TABLE *S,EFI_DRIVER_BINDING_PROTOCOL *B,EFI_HANDLE BH,CONST EFI_COMPONENT_NAME_PROTOCOL *C,CONST EFI_COMPONENT_NAME2_PROTOCOL *C2){return EFI_SUCCESS;}
VOID PrintCSD(UINT32 *P){}VOID PrintRCA(UINT32 P){}VOID PrintOCR(UINT32 P){}VOID PrintResponseR1(UINT32 P){}VOID PrintCID(UINT32 *P){}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL N){assert(N>=Tpl);EFI_TPL O=Tpl;Tpl=N;return O;}
STATIC VOID EFIAPI Restore(EFI_TPL N){assert(N<=Tpl);Tpl=N;}
STATIC UINT32 Reg(UINT32 O,UINT32 W){UINT32 V=0;memcpy(&V,Registers+O,W);return V;}
STATIC VOID Put(UINT32 O,UINT32 W,UINT32 V){memcpy(Registers+O,&V,W);}
STATIC UINT32 ReadRegister(UINTN A,UINT32 W){assert(Live&&A>=CR_SD_NATIVE_BASE&&A+W<=CR_SD_NATIVE_BASE+sizeof(Registers));UINT32 O=A-CR_SD_NATIVE_BASE;
 if(O==0x20||O==0x22){assert(Position+W<=Length);UINT32 V=0;memcpy(&V,Fifo+Position,W);Position+=W;if(Position==Length){Put(0x30,2,Reg(0x30,2)|2);Put(0x24,4,0);}return V;}return Reg(O,W);}
UINT8 EFIAPI MmioRead8(UINTN A){return ReadRegister(A,1);}UINT16 EFIAPI MmioRead16(UINTN A){return ReadRegister(A,2);}UINT32 EFIAPI MmioRead32(UINTN A){return ReadRegister(A,4);}
STATIC VOID Command(UINT32 Value){++Commands;Index=Value>>8;if(RemoveCommand&&Index==RemoveCommand){Card=FALSE;return;}Argument=Reg(8,4);if(FailOcrCommand&&Index==FailOcrCommand){Put(0x30,2,0x8000);Put(0x32,2,BIT0);return;}BOOLEAN App=Application;Application=FALSE;UINT32 Reply[4]={0};
 Position=0;Length=Value&BIT5?Reg(4,2):0;assert(!(Reg(0xc,2)&BIT0));
 if(Index==55){Reply[0]=MMC_STATUS_APP_CMD;Application=TRUE;}
 else if(Index==8)Reply[0]=Argument;
 else if(Index==41){assert(App);++OcrCalls;Reply[0]=(OcrCalls>OcrBusyReplies?BIT31:0)|BIT30|0x00ff8000;}
 else if(Index==3){assert(Argument==0);Reply[0]=0x12340000;}
 else if(Index==13)Reply[0]=MMC_R0_READY_FOR_DATA|(MMC_R0_STATE_TRAN<<9);
 else if(Index==6&&App&&RejectWidth)Reply[0]=BIT22;
 else if(Index==9){/* Complete SD CSDv2: C_SIZE=0x1ffff,512-byte blocks,CCC SWITCH. */UINT32 Full[4]={0,0xffff0000,0x40090001,0x40000000};for(UINT32 I=0;I<4;++I)Reply[I]=(Full[I]>>8)|(I==3?0:Full[I+1]<<24);}
 for(UINT32 I=0;I<4;++I)Put(0x10+I*4,4,Reply[I]);
 if(Length){++DataCommands;if(Index==FailDataCommand){Put(0x30,2,0x8000);Put(0x32,2,BIT1);return;}
   memset(Fifo,0,sizeof(Fifo));if(Index==51){assert(App&&Length==8);Fifo[0]=2;Fifo[1]=5;}
   else if(Index==6){assert(!App&&Length==64);Fifo[14]=2;if(SwitchAccepted)Fifo[19]=1;if(Argument&BIT31)CardHighSpeed=SwitchAccepted;}
   else if(Index==17){assert(Argument<8&&Length==512);memcpy(Fifo,Storage[Argument],512);}
 }
 UINT32 Events=1;if(Length){BOOLEAN Read=(Reg(0xc,2)&BIT4)!=0;Events|=Read?0x20:0x10;Put(0x24,4,Read?BIT11:BIT10);}if((Value&3)==3)Events|=2;Put(0x30,2,Events);
}
STATIC VOID WriteRegister(UINTN A,UINT32 W,UINT32 V){assert(Live&&A>=CR_SD_NATIVE_BASE&&A+W<=CR_SD_NATIVE_BASE+sizeof(Registers));UINT32 O=A-CR_SD_NATIVE_BASE;
 if(O==0x34||O==0x38)V&=0x7fffU;
 if(O==0x20||O==0x22){assert(Index==24&&Argument<8&&Position+W<=Length);memcpy(Fifo+Position,&V,W);Position+=W;if(Position==Length){memcpy(Storage[Argument],Fifo,512);Put(0x30,2,Reg(0x30,2)|2);Put(0x24,4,0);}return;}
 if(O==0x30||O==0x32){Put(O,W,Reg(O,W)&~V);Put(0x30,2,(Reg(0x30,2)&0x7fffU)|((Reg(0x32,2)&0x1ffU)?0x8000U:0));return;}if(O==0x248){Put(0x240,4,Reg(0x240,4)&~V);return;}
 if(O==0x2f){if(V==1){++FullResets;Put(0x29,1,0);Put(0x2c,2,0);}Put(0x2f,1,0);Put(0x24,4,0);Put(0x30,2,0);Put(0x32,2,0);return;}
 if(O==0x2c&&(V&1))V|=2;if(O==0x29)Put(0x240,4,V?CR_SDHC_POWER_ON:CR_SDHC_POWER_OFF);Put(O,W,V);if(O==0xe)Command(V);
}
UINT8 EFIAPI MmioWrite8(UINTN A,UINT8 V){WriteRegister(A,1,V);return V;}UINT16 EFIAPI MmioWrite16(UINTN A,UINT16 V){WriteRegister(A,2,V);return V;}UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){WriteRegister(A,4,V);return V;}
EFI_STATUS CrSdResourcesAcquire(CR_SDHC *C){assert(Tpl<=TPL_CALLBACK&&C->Initialized);Live=TRUE;return EFI_SUCCESS;}
EFI_STATUS CrSdResourcesReady(VOID){return Live?EFI_SUCCESS:EFI_NOT_READY;}
EFI_STATUS CrSdResourcesSetClock(UINT32 Hz){assert(Tpl<=TPL_CALLBACK);ClockRate=Hz;return EFI_SUCCESS;}
EFI_STATUS CrSdResourcesCardPresent(BOOLEAN *P){assert(Tpl<=TPL_CALLBACK);*P=Card;return Live?EFI_SUCCESS:EFI_NOT_READY;}
EFI_STATUS CrSdResourcesServicePower(CR_SDHC *C){assert(Tpl<=TPL_CALLBACK);++PowerServices;UINT32 P=0;EFI_STATUS S=CrSdhcPowerCapture(C,&P);return S==EFI_SUCCESS&&P?CrSdhcPowerComplete(C,P,TRUE,TRUE):S;}
EFI_STATUS CrSdResourcesRequestPower(CR_SDHC *C,BOOLEAN On){EFI_STATUS S=CrSdResourcesServicePower(C);if(S==EFI_SUCCESS)S=CrSdhcPowerRequest(C,On);return S==EFI_SUCCESS?CrSdResourcesServicePower(C):S;}
EFI_STATUS CrSdResourcesRelease(CR_SDHC *C){Live=FALSE;return EFI_SUCCESS;}
EFI_STATUS CrSdResourcesFreeze(CR_SDHC *C){return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **Out,EFI_HANDLE A,EFI_HANDLE C,UINT32 Attr){assert(H==HostHandle&&G==&gEmbeddedMmcHostProtocolGuid&&Attr==EFI_OPEN_PROTOCOL_BY_DRIVER);if(Opened)return EFI_ALREADY_STARTED;Opened=TRUE;*Out=&mSdHost.Host;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C){assert(H==HostHandle&&Opened);Opened=FALSE;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){va_list A;va_start(A,H);EFI_GUID *G=va_arg(A,EFI_GUID *);if(G==&gEmbeddedMmcHostProtocolGuid){assert(FullResets&&va_arg(A,EFI_MMC_HOST_PROTOCOL *)==&mSdHost.Host);*H=HostHandle;}else{assert(G==&gEfiBlockIoProtocolGuid&&!Block);Block=va_arg(A,EFI_BLOCK_IO_PROTOCOL *);*H=BlockHandle;}va_end(A);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){assert(H==BlockHandle&&Block);Block=NULL;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Reinstall(EFI_HANDLE H,EFI_GUID *G,VOID *Old,VOID *New);
STATIC EFI_STATUS EFIAPI CreateEx(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){ExitCallback=Fn;*Out=(VOID *)0xf0;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI CloseEvent(EFI_EVENT E){return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI StallBoot(UINTN Us){assert(Tpl<=TPL_CALLBACK);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *D,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN R);
#include "../Driver/SdhciCrDxe/MmcHost.c"
#include "../Driver/SdhciCrDxe/SdhciCrDxe.c"
#include "MmcClassProduction.inc"
STATIC EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *D,EFI_DEVICE_PATH_PROTOCOL *P,BOOLEAN R){assert(H==HostHandle);return MmcDriverBindingStart(&gMmcDriverBinding,H,NULL);}
STATIC EFI_STATUS EFIAPI Reinstall(EFI_HANDLE H,EFI_GUID *G,VOID *Old,VOID *New){
 assert(H==BlockHandle&&Old==Block&&New==Block);
 /* CoreReinstallProtocolInterface synchronously reconnects storage drivers.
  * Those consumers must be able to read the partition sector before return. */
 UINT32 Sector[128];EFI_STATUS S=Block->ReadBlocks(Block,Block->Media->MediaId,0,sizeof(Sector),Sector);
 assert(S==(Card?EFI_SUCCESS:EFI_NO_MEDIA));
 if(Card)assert(!memcmp(Sector,Storage[0],sizeof(Sector)));
 /* The scan still owns instance lifetime while allowing consumer I/O. */
 assert(MmcDriverBindingStop(&gMmcDriverBinding,HostHandle,0,NULL)==EFI_NOT_READY);
 if(ResetDuringReinstall){ResetDuringReinstall=FALSE;Card=FALSE;assert(Block->Reset(Block,FALSE)==EFI_SUCCESS);}
 return ReinstallStatus;
}
STATIC VOID Setup(VOID){assert(!Pools&&!Block&&!Opened);ZeroMem(&mSdHost,sizeof(mSdHost));ZeroMem(Registers,sizeof(Registers));for(UINT32 I=0;I<8;++I)for(UINT32 J=0;J<512;++J)Storage[I][J]=(UINT8)(I+J*3);
 ReinstallStatus=EFI_SUCCESS;ResetDuringReinstall=FALSE;
 RemoveCommand=FailOcrCommand=OcrBusyReplies=OcrCalls=0;Position=Length=Index=Argument=Commands=DataCommands=FailDataCommand=FullResets=PowerServices=ClockRate=0;Tpl=TPL_APPLICATION;Card=SwitchAccepted=TRUE;Live=Application=CardHighSpeed=RejectWidth=FALSE;
 Boot=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.OpenProtocol=Open,.CloseProtocol=CloseProtocol,.InstallMultipleProtocolInterfaces=Install,.UninstallMultipleProtocolInterfaces=Uninstall,.ReinstallProtocolInterface=Reinstall,.CreateEventEx=CreateEx,.CloseEvent=CloseEvent,.ConnectController=Connect,.Stall=StallBoot};gBS=&Boot;InitializeMmcHostPool();gMmcDriverBinding.DriverBindingHandle=(VOID *)0xa0;}
STATIC VOID Done(VOID){assert(MmcDriverBindingStop(&gMmcDriverBinding,HostHandle,0,NULL)==EFI_SUCCESS);assert(!Pools&&!Block&&!Opened&&Tpl==TPL_APPLICATION);}
int main(void){
 /* Command errors terminate this identification attempt, not 100 OCR retries.
  * A genuinely busy, valid OCR is still polled until ready. */
 for(UINT32 Fail=41;Fail<=55;Fail+=14){Setup();FailOcrCommand=Fail;
  assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block&&!Block->Media->MediaPresent);
  assert(Commands<8&&Index==Fail);FailOcrCommand=0;CheckCardsCallback(NULL,NULL);
  assert(Block->Media->MediaPresent);Done();}
 Setup();OcrBusyReplies=3;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block->Media->MediaPresent&&OcrCalls==4);Done();
 for(UINT32 Remove=8;Remove<=55;Remove+=Remove==8?33:14){Setup();RemoveCommand=Remove;
  assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block&&!Block->Media->MediaPresent);
  assert(Index==Remove&&Commands<8);Done();}
 Setup();Card=FALSE;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block&&!Block->Media->MediaPresent&&!Commands&&!PowerServices);
 Card=TRUE;Tpl=TPL_CALLBACK;CheckCardsCallback(NULL,NULL);Tpl=TPL_APPLICATION;assert(Block->Media->MediaPresent&&Block->Media->LastBlock==134217727ULL&&ClockRate==50000000&&CardHighSpeed);
 UINT32 Data[256],Write[128];memset(Data,0xa5,sizeof(Data));assert(Block->ReadBlocks(Block,Block->Media->MediaId,2,sizeof(Data),Data)==EFI_SUCCESS);assert(!memcmp(Data,Storage[2],512)&&!memcmp((UINT8 *)Data+512,Storage[3],512));
 memset(Write,0x37,sizeof(Write));assert(Block->WriteBlocks(Block,Block->Media->MediaId,4,sizeof(Write),Write)==EFI_SUCCESS&&!memcmp(Storage[4],Write,512));
 FailDataCommand=17;assert(Block->ReadBlocks(Block,Block->Media->MediaId,2,512,Data)==EFI_DEVICE_ERROR);FailDataCommand=0;
 UINT32 Media=Block->Media->MediaId;Card=FALSE;CheckCardsCallback(NULL,NULL);assert(!Block->Media->MediaPresent&&Block->Media->MediaId!=Media);Card=TRUE;CheckCardsCallback(NULL,NULL);assert(Block->Media->MediaPresent);Done();
 Setup();FailDataCommand=51;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block&&!Block->Media->MediaPresent);FailDataCommand=0;CheckCardsCallback(NULL,NULL);assert(Block->Media->MediaPresent&&ClockRate==50000000);Done();
 Setup();SwitchAccepted=FALSE;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block&&!Block->Media->MediaPresent);for(UINT32 I=0;I<12;++I)CheckCardsCallback(NULL,NULL);UINT32 Before=Commands;SwitchAccepted=TRUE;CheckCardsCallback(NULL,NULL);assert(Commands==Before&&!Block->Media->MediaPresent);Card=FALSE;CheckCardsCallback(NULL,NULL);Card=TRUE;CheckCardsCallback(NULL,NULL);assert(Block->Media->MediaPresent&&CardHighSpeed);Done();
 Setup();RejectWidth=TRUE;assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block&&!Block->Media->MediaPresent);RejectWidth=FALSE;CheckCardsCallback(NULL,NULL);assert(Block->Media->MediaPresent);Done();
 Setup();assert(SdhciCrDxeEntryPoint(NULL,NULL)==EFI_SUCCESS&&Block->Media->MediaPresent);
 MMC_HOST_INSTANCE *Instance=FindMmcHost(HostHandle);assert(Instance&&!Instance->ChangePending);
 ReinstallStatus=EFI_ACCESS_DENIED;Instance->ChangePending=TRUE;CheckCardsCallback(NULL,NULL);
 assert(Instance->ChangePending&&!Instance->Busy&&Instance->Initialized);
 ReinstallStatus=EFI_SUCCESS;CheckCardsCallback(NULL,NULL);assert(!Instance->ChangePending);
 ResetDuringReinstall=TRUE;Instance->ChangePending=TRUE;CheckCardsCallback(NULL,NULL);
 assert(!Instance->Initialized&&Instance->ChangePending&&!Block->Media->MediaPresent);
 CheckCardsCallback(NULL,NULL);assert(!Instance->ChangePending);Done();
 puts("PASS actual MMC+nativeSDHCI+PIO core: nested reconnect reads, retained notification/reentrant-stop exclusion,  no-card hotplug, SDv2 full initialization, SCR/switch/R2 capacity, multi-sector single-block reads/write, error/retry budget and TPL contract");return 0;}
