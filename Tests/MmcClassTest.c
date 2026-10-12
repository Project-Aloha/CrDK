/* Actual MMC class with independent SD command/data and EFI lifetime models.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "Mmc.h"
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DevicePathLib.h>

static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
EFI_GUID gEfiBlockIoProtocolGuid,gEfiDevicePathProtocolGuid,gEmbeddedMmcHostProtocolGuid,gEfiDriverDiagnostics2ProtocolGuid;
EFI_COMPONENT_NAME_PROTOCOL gMmcComponentName;
EFI_COMPONENT_NAME2_PROTOCOL gMmcComponentName2;
EFI_DRIVER_DIAGNOSTICS2_PROTOCOL gMmcDriverDiagnostics2;
static EFI_TPL Tpl=TPL_APPLICATION;
static UINTN Pools,Allocations,FailAllocation,Uninstalls,Closes,Transfers,StatusQueries,InitAttempts,Reinstalls,CmdCount;
static UINT32 Commands[12032],Arguments[12032],ReadyResponse;
static BOOLEAN Present,ReadOnly,FourBit,HighSpeed,SwitchAccepted,MultiBlock,NestedStop,Streaming,NestedRead;
static EFI_STATUS InitStatus,ReceiveStatus,ReadStatus,UninstallStatus,CloseStatus,NestedStatus,InstallStatus;
static UINTN FailRead,FailReceiveAfter,FailCommand;
static UINTN OpCondBusy,OpCondReplies,IdleUs;
static UINT32 LastCmd,LastArg,CapacitySize,IosWidth,IosSpeed;
static EFI_MMC_HOST_PROTOCOL Host;
static EFI_BLOCK_IO_PROTOCOL *Published[4];
static EFI_HANDLE Controllers[4];
static BOOLEAN Opened[4];

VOID *EFIAPI ZeroMem(VOID *P,UINTN N){return memset(P,0,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI AllocatePool(UINTN N){if(++Allocations==FailAllocation)return NULL;VOID *P=malloc(N);if(P)++Pools;return P;}
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=AllocatePool(N);if(P)memset(P,0,N);return P;}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *S){VOID *P=AllocatePool(N);if(P)memcpy(P,S,N);return P;}
VOID EFIAPI FreePool(VOID *P){assert(P&&Pools);--Pools;free(P);}
VOID *EFIAPI AllocatePages(UINTN N){return AllocateZeroPool(N*4096);}
VOID EFIAPI FreePages(VOID *P,UINTN N){FreePool(P);}
UINT64 EFIAPI MultU64x32(UINT64 V,UINT32 M){return V*M;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){ }
UINTN EFIAPI Print(CONST CHAR16 *F,...){return 0;}
LIST_ENTRY *EFIAPI InitializeListHead(LIST_ENTRY *H){H->ForwardLink=H->BackLink=H;return H;}
LIST_ENTRY *EFIAPI InsertTailList(LIST_ENTRY *H,LIST_ENTRY *P){P->BackLink=H->BackLink;P->ForwardLink=H;H->BackLink->ForwardLink=P;H->BackLink=P;return H;}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *P){P->BackLink->ForwardLink=P->ForwardLink;P->ForwardLink->BackLink=P->BackLink;return P->ForwardLink;}
BOOLEAN EFIAPI IsDevicePathEnd(CONST VOID *P){CONST EFI_DEVICE_PATH_PROTOCOL *D=P;return D->Type==END_DEVICE_PATH_TYPE;}
VOID EFIAPI SetDevicePathEndNode(VOID *P){EFI_DEVICE_PATH_PROTOCOL *D=P;*D=(EFI_DEVICE_PATH_PROTOCOL){END_DEVICE_PATH_TYPE,END_ENTIRE_DEVICE_PATH_SUBTYPE,{4,0}};}
UINTN EFIAPI DevicePathNodeLength(CONST VOID *P){CONST EFI_DEVICE_PATH_PROTOCOL *D=P;return D->Length[0]|(D->Length[1]<<8);}
EFI_DEVICE_PATH_PROTOCOL *EFIAPI AppendDevicePathNode(CONST EFI_DEVICE_PATH_PROTOCOL *D,CONST EFI_DEVICE_PATH_PROTOCOL *N){return AllocateCopyPool(sizeof(*N),N);}
EFI_STATUS EFIAPI EfiLibInstallDriverBindingComponentName2(EFI_HANDLE H,CONST EFI_SYSTEM_TABLE *S,EFI_DRIVER_BINDING_PROTOCOL *B,EFI_HANDLE BH,CONST EFI_COMPONENT_NAME_PROTOCOL *C,CONST EFI_COMPONENT_NAME2_PROTOCOL *C2){return EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL N){EFI_TPL O=Tpl;assert(N>=O);Tpl=N;return O;}
static VOID EFIAPI Restore(EFI_TPL N){assert(N<=Tpl);Tpl=N;}
static UINTN Slot(EFI_HANDLE H){for(UINTN I=0;I<4;++I)if(Controllers[I]==H)return I;assert(0);return 0;}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **Out,EFI_HANDLE A,EFI_HANDLE C,UINT32 F){UINTN I=Slot(H);if(Opened[I])return EFI_ALREADY_STARTED;Opened[I]=TRUE;*Out=&Host;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE A,EFI_HANDLE C){++Closes;UINTN I=Slot(H);assert(Opened[I]);if(CloseStatus!=EFI_SUCCESS)return CloseStatus;Opened[I]=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){if(InstallStatus!=EFI_SUCCESS)return InstallStatus;va_list A;va_start(A,H);assert(va_arg(A,EFI_GUID *)==&gEfiBlockIoProtocolGuid);EFI_BLOCK_IO_PROTOCOL *B=va_arg(A,EFI_BLOCK_IO_PROTOCOL *);va_end(A);UINTN I=0;while(I<4&&Published[I])++I;assert(I<4);Published[I]=B;*H=(EFI_HANDLE)(UINTN)(0x50+I);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...);
static EFI_STATUS EFIAPI Reinstall(EFI_HANDLE H,EFI_GUID *G,VOID *Old,VOID *New){++Reinstalls;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Stall(UINTN N){assert(Tpl<=TPL_CALLBACK);IdleUs+=N;return EFI_SUCCESS;}
static BOOLEAN EFIAPI Card(EFI_MMC_HOST_PROTOCOL *H){return Present;}
static BOOLEAN EFIAPI Protect(EFI_MMC_HOST_PROTOCOL *H){return ReadOnly;}
static BOOLEAN EFIAPI Multi(EFI_MMC_HOST_PROTOCOL *H){return MultiBlock;}
static EFI_STATUS EFIAPI Path(EFI_MMC_HOST_PROTOCOL *H,EFI_DEVICE_PATH_PROTOCOL **Out){*Out=AllocateZeroPool(sizeof(VENDOR_DEVICE_PATH));if(!*Out)return EFI_OUT_OF_RESOURCES;(*Out)->Type=HARDWARE_DEVICE_PATH;(*Out)->SubType=HW_VENDOR_DP;(*Out)->Length[0]=sizeof(VENDOR_DEVICE_PATH);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Notify(EFI_MMC_HOST_PROTOCOL *H,MMC_STATE State){assert(Tpl<=TPL_CALLBACK);if(State==MmcHwInitializationState){++InitAttempts;return InitStatus;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Ios(EFI_MMC_HOST_PROTOCOL *H,UINT32 Speed,UINT32 Width,UINT32 Timing){IosWidth=Width;IosSpeed=Speed;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Send(EFI_MMC_HOST_PROTOCOL *H,MMC_CMD C,UINT32 Arg){LastCmd=MMC_GET_INDX(C);LastArg=Arg;assert(CmdCount<12032);Commands[CmdCount]=LastCmd;Arguments[CmdCount++]=Arg;if(FailCommand&&LastCmd==FailCommand)return EFI_DEVICE_ERROR;if(LastCmd==1||LastCmd==5)return EFI_UNSUPPORTED;if(LastCmd==13)++StatusQueries;if(LastCmd==18||LastCmd==25)Streaming=TRUE;if(LastCmd==12)Streaming=FALSE;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Receive(EFI_MMC_HOST_PROTOCOL *H,MMC_RESPONSE_TYPE Type,UINT32 *Out){if(ReceiveStatus!=EFI_SUCCESS&&(FailReceiveAfter==0||Transfers>=FailReceiveAfter))return ReceiveStatus;memset(Out,0,(Type==MMC_RESPONSE_TYPE_R2?4:1)*sizeof(*Out));switch(LastCmd){case 8:Out[0]=LastArg;break;case 55:Out[0]=MMC_STATUS_APP_CMD;break;case 41:Out[0]=(OpCondReplies++<OpCondBusy?0:BIT31)|BIT30|0x00ff8000;break;case 3:assert(LastArg==0);Out[0]=0x12340000;break;case 9:Out[1]=(CapacitySize&0xffff)<<16;Out[2]=(CapacitySize>>16)|(9U<<16)|(HighSpeed?(1U<<30):0);Out[3]=1U<<30;break;case 13:Out[0]=Streaming?(MMC_R0_STATE_DATA<<9):ReadyResponse;break;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Read(EFI_MMC_HOST_PROTOCOL *H,EFI_LBA Lba,UINTN Bytes,UINT32 *Out){assert(Tpl<=TPL_CALLBACK);++Transfers;if(NestedRead){NestedRead=FALSE;extern EFI_DRIVER_BINDING_PROTOCOL gMmcDriverBinding;extern EFI_STATUS EFIAPI MmcDriverBindingStop(EFI_DRIVER_BINDING_PROTOCOL*,EFI_HANDLE,UINTN,EFI_HANDLE*);NestedStatus=MmcDriverBindingStop(&gMmcDriverBinding,Controllers[0],0,NULL);}if(Transfers==FailRead)return ReadStatus;memset(Out,0,Bytes);if(LastCmd==51){((UINT8 *)Out)[0]=2;((UINT8 *)Out)[1]=FourBit?5:1;}else if(LastCmd==6){if(HighSpeed)Out[3]=SD_HIGH_SPEED_SUPPORTED;if(SwitchAccepted)Out[4]=0x01000000;}return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Write(EFI_MMC_HOST_PROTOCOL *H,EFI_LBA Lba,UINTN Bytes,UINT32 *Out){return Read(H,Lba,Bytes,Out);}
VOID PrintCSD(UINT32 *P){} VOID PrintRCA(UINT32 P){} VOID PrintOCR(UINT32 P){} VOID PrintResponseR1(UINT32 P){} VOID PrintCID(UINT32 *P){}
#include "MmcClassProduction.inc"
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){++Uninstalls;if(UninstallStatus!=EFI_SUCCESS)return UninstallStatus;UINTN I=(UINTN)H-0x50;assert(I<4&&Published[I]);if(NestedStop){NestedStop=FALSE;NestedStatus=MmcDriverBindingStop(&gMmcDriverBinding,Controllers[I],0,NULL);}Published[I]=NULL;return EFI_SUCCESS;}
static VOID Reset(VOID){assert(!Pools);for(UINTN I=0;I<4;++I)assert(!Opened[I]&&!Published[I]);Tpl=TPL_APPLICATION;Allocations=FailAllocation=Uninstalls=Closes=Transfers=StatusQueries=InitAttempts=Reinstalls=CmdCount=0;Present=TRUE;ReadOnly=FALSE;FourBit=TRUE;HighSpeed=FALSE;SwitchAccepted=TRUE;Streaming=MultiBlock=NestedStop=NestedRead=FALSE;InitStatus=ReceiveStatus=ReadStatus=UninstallStatus=CloseStatus=InstallStatus=EFI_SUCCESS;FailRead=FailReceiveAfter=FailCommand=OpCondBusy=OpCondReplies=IdleUs=0;CapacitySize=0x1ffff;ReadyResponse=MMC_R0_READY_FOR_DATA|(MMC_R0_STATE_TRAN<<9);LastCmd=LastArg=IosWidth=IosSpeed=0;for(UINTN I=0;I<4;++I)Controllers[I]=(EFI_HANDLE)(UINTN)(0x10+I);Services=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.OpenProtocol=Open,.CloseProtocol=Close,.InstallMultipleProtocolInterfaces=Install,.UninstallMultipleProtocolInterfaces=Uninstall,.ReinstallProtocolInterface=Reinstall,.Stall=Stall};Host=(EFI_MMC_HOST_PROTOCOL){MMC_HOST_PROTOCOL_REVISION,Card,Protect,Path,Notify,Send,Receive,Read,Write,Ios,Multi};InitializeMmcHostPool();gMmcDriverBinding.DriverBindingHandle=(EFI_HANDLE)0x42;}
static EFI_STATUS Start(UINTN I){return MmcDriverBindingStart(&gMmcDriverBinding,Controllers[I],NULL);}
static EFI_STATUS Stop(UINTN I){return MmcDriverBindingStop(&gMmcDriverBinding,Controllers[I],0,NULL);}
static MMC_HOST_INSTANCE *Device(UINTN I){assert(Published[I]);return MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS(Published[I]);}
static VOID Capacity(VOID){Reset();assert(Start(0)==EFI_SUCCESS);assert(Device(0)->Initialized&&Published[0]->Media->MediaPresent);assert(Published[0]->Media->LastBlock==134217727ULL);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID TransferError(VOID){Reset();assert(Start(0)==EFI_SUCCESS);UINT32 Data[128];Transfers=CmdCount=0;FailRead=1;ReadStatus=EFI_DEVICE_ERROR;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,0,sizeof(Data),Data)==EFI_DEVICE_ERROR);assert(Transfers==1);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID Lifecycle(VOID){Reset();Present=FALSE;assert(Start(0)==EFI_SUCCESS);assert(Start(1)==EFI_SUCCESS);assert(Stop(1)==EFI_SUCCESS);assert(Published[0]&&!Published[1]&&Opened[0]&&!Opened[1]);assert(Stop(0)==EFI_SUCCESS);assert(!Pools);Reset();}
static VOID Hotplug(VOID){Reset();Present=FALSE;assert(Start(0)==EFI_SUCCESS);assert(!InitAttempts&&!Published[0]->Media->MediaPresent);Present=TRUE;InitStatus=EFI_DEVICE_ERROR;CheckCardsCallback(NULL,NULL);assert(InitAttempts==1&&!Device(0)->Initialized&&!Published[0]->Media->MediaPresent);InitStatus=EFI_SUCCESS;CheckCardsCallback(NULL,NULL);assert(InitAttempts==2&&Device(0)->Initialized&&Published[0]->Media->MediaPresent);UINT32 Id=Published[0]->Media->MediaId;Present=FALSE;CheckCardsCallback(NULL,NULL);assert(!Device(0)->Initialized&&!Published[0]->Media->MediaPresent&&Published[0]->Media->MediaId!=Id);Present=TRUE;CheckCardsCallback(NULL,NULL);assert(Device(0)->Initialized&&InitAttempts==3);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID BusWidth(VOID){Reset();FourBit=FALSE;assert(Start(0)==EFI_SUCCESS);assert(IosWidth==1&&IosSpeed==25000000);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID Ready(VOID){Reset();assert(Start(0)==EFI_SUCCESS);UINT32 Data[128];CmdCount=StatusQueries=Transfers=0;ReadyResponse=MMC_R0_STATE_TRAN<<9;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,0,sizeof(Data),Data)==EFI_NOT_READY);assert(!Transfers&&StatusQueries==20);CmdCount=StatusQueries=0;ReadyResponse=MMC_R0_READY_FOR_DATA|(7U<<9);assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,0,sizeof(Data),Data)==EFI_NOT_READY);assert(!Transfers&&StatusQueries==20);CmdCount=0;ReceiveStatus=EFI_DEVICE_ERROR;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,0,sizeof(Data),Data)==EFI_DEVICE_ERROR);assert(!Transfers);ReceiveStatus=EFI_SUCCESS;assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID Cleanup(VOID){Reset();Present=FALSE;assert(Start(0)==EFI_SUCCESS);UINTN P=Pools;EFI_BLOCK_IO_PROTOCOL *B=Published[0];UninstallStatus=EFI_ACCESS_DENIED;assert(Stop(0)==EFI_ACCESS_DENIED&&Pools==P&&Published[0]==B&&Opened[0]);UINT32 Data[128];assert(B->ReadBlocks(B,B->Media->MediaId,0,sizeof(Data),Data)==EFI_NOT_READY);UninstallStatus=EFI_SUCCESS;CloseStatus=EFI_DEVICE_ERROR;assert(Stop(0)==EFI_DEVICE_ERROR&&Pools==P&&!Published[0]&&Opened[0]);CloseStatus=EFI_SUCCESS;assert(Stop(0)==EFI_SUCCESS);Reset();Present=FALSE;assert(Start(0)==EFI_SUCCESS);NestedStop=TRUE;assert(Stop(0)==EFI_SUCCESS&&NestedStatus==EFI_NOT_READY);Reset();
 for(UINTN I=1;I<=4;++I){Reset();Present=FALSE;FailAllocation=I;assert(Start(0)==EFI_OUT_OF_RESOURCES&&!Pools&&!Opened[0]&&!Published[0]&&Tpl==TPL_APPLICATION);}
 Reset();Present=FALSE;FailAllocation=2;CloseStatus=EFI_DEVICE_ERROR;assert(Start(0)==EFI_OUT_OF_RESOURCES&&Pools&&Opened[0]&&!Published[0]);FailAllocation=0;CloseStatus=EFI_SUCCESS;assert(Stop(0)==EFI_SUCCESS);Reset();Present=FALSE;InstallStatus=EFI_DEVICE_ERROR;CloseStatus=EFI_DEVICE_ERROR;assert(Start(0)==EFI_DEVICE_ERROR&&Pools&&Opened[0]&&!Published[0]);InstallStatus=CloseStatus=EFI_SUCCESS;assert(Start(0)==EFI_SUCCESS&&Published[0]&&Opened[0]);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID SingleChunks(VOID){Reset();assert(Start(0)==EFI_SUCCESS);UINT32 Data[384];CmdCount=Transfers=0;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,4,sizeof(Data),Data)==EFI_SUCCESS);assert(Transfers==3);UINTN DataCmds=0;for(UINTN I=0;I<CmdCount;++I){assert(Commands[I]!=18&&Commands[I]!=12);if(Commands[I]==17)assert(Arguments[I]==4+DataCmds++);}assert(DataCmds==3);CmdCount=Transfers=0;FailRead=2;ReadStatus=EFI_TIMEOUT;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,4,sizeof(Data),Data)==EFI_TIMEOUT&&Transfers==2);FailRead=0;CmdCount=0;NestedRead=TRUE;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,4,512,Data)==EFI_SUCCESS&&NestedStatus==EFI_NOT_READY&&Published[0]&&Opened[0]);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID MultiOrdering(VOID){Reset();MultiBlock=TRUE;assert(Start(0)==EFI_SUCCESS);UINT32 Data[256];CmdCount=Transfers=0;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,4,sizeof(Data),Data)==EFI_SUCCESS&&Transfers==1&&!Streaming);assert(CmdCount==4&&Commands[0]==13&&Commands[1]==18&&Commands[2]==12&&Commands[3]==13);CmdCount=Transfers=0;FailCommand=12;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,4,sizeof(Data),Data)==EFI_DEVICE_ERROR);FailCommand=0;assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID Bounds(VOID){Reset();assert(Start(0)==EFI_SUCCESS);UINT32 Data[256];CmdCount=Transfers=0;assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,MAX_UINT64,512,Data)==EFI_INVALID_PARAMETER&&!CmdCount);assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,Published[0]->Media->LastBlock,sizeof(Data),Data)==EFI_INVALID_PARAMETER&&!CmdCount);assert(Published[0]->ReadBlocks(Published[0],Published[0]->Media->MediaId,0,513,Data)==EFI_BAD_BUFFER_SIZE&&!CmdCount);assert(Stop(0)==EFI_SUCCESS);Reset();HighSpeed=TRUE;SwitchAccepted=FALSE;assert(Start(0)==EFI_SUCCESS&&!Device(0)->Initialized&&!Published[0]->Media->MediaPresent&&!IosWidth);SwitchAccepted=TRUE;CheckCardsCallback(NULL,NULL);assert(Device(0)->Initialized&&IosSpeed==50000000);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID RetryBudget(VOID){Reset();InitStatus=EFI_TIMEOUT;assert(Start(0)==EFI_SUCCESS);for(UINTN I=0;I<20;++I)CheckCardsCallback(NULL,NULL);assert(InitAttempts==3&&!Device(0)->Initialized&&!Published[0]->Media->MediaPresent);InitStatus=EFI_SUCCESS;CheckCardsCallback(NULL,NULL);assert(InitAttempts==3&&!Device(0)->Initialized);Present=FALSE;CheckCardsCallback(NULL,NULL);Present=TRUE;CheckCardsCallback(NULL,NULL);assert(InitAttempts==4&&Device(0)->Initialized&&Published[0]->Media->MediaPresent);assert(Stop(0)==EFI_SUCCESS);Reset();}
static VOID OpCond(VOID){Reset();OpCondBusy=30;assert(Start(0)==EFI_SUCCESS&&Device(0)->Initialized&&OpCondReplies==31&&IdleUs==300000);assert(Stop(0)==EFI_SUCCESS);Reset();OpCondBusy=10000;assert(Start(0)==EFI_SUCCESS&&!Device(0)->Initialized&&OpCondReplies==100&&IdleUs==1000000);for(UINTN I=0;I<10;++I)CheckCardsCallback(NULL,NULL);assert(InitAttempts==3&&OpCondReplies==300&&IdleUs==3000000&&!Published[0]->Media->MediaPresent);assert(Stop(0)==EFI_SUCCESS);Reset();}
int main(int argc,char **argv){const char *Mode=argc>1?argv[1]:"all";if(!strcmp(Mode,"capacity")){Capacity();return 0;}if(!strcmp(Mode,"transfer")){TransferError();return 0;}if(!strcmp(Mode,"lifecycle")){Lifecycle();return 0;}if(!strcmp(Mode,"hotplug")){Hotplug();return 0;}if(!strcmp(Mode,"width")){BusWidth();return 0;}if(!strcmp(Mode,"ready")){Ready();return 0;}if(!strcmp(Mode,"cleanup")){Cleanup();return 0;}if(!strcmp(Mode,"chunks")){SingleChunks();return 0;}if(!strcmp(Mode,"multi")){MultiOrdering();return 0;}if(!strcmp(Mode,"bounds")){Bounds();return 0;}Capacity();TransferError();Lifecycle();Hotplug();BusWidth();Ready();Cleanup();SingleChunks();MultiOrdering();Bounds();RetryBudget();OpCond();puts("PASS actual MMC class capacity, transfer/status errors, exact retained/reentrant teardown, allocation faults, bounded hotplug/OCR retry, bus width, single/multi sequencing and bounds");return 0;}
