/** Actual battery DXE + portable core lifecycle. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../Driver/BatteryChargerCrDxe/BatteryChargerCrDxe.c"
EFI_GUID gEfiPmicGlinkCrProtocolGuid, gEfiBatteryChargerCrProtocolGuid, gEfiEventExitBootServicesGuid;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static EFI_PMIC_GLINK_CR_PROTOCOL Provider;
static EFI_PMIC_GLINK_RECEIVE Rx;
static EFI_PMIC_GLINK_STATE_CHANGE Changed;
static VOID *Client;
static EFI_PMIC_GLINK_CR_STATE LinkState;
static EFI_EVENT_NOTIFY TimerFn,ExitFn;
static BOOLEAN TimerOpen,ExitOpen,Published,Registered,OnlineAtRegister,ImmediateReply;
static UINT32 Sends,Creates,FailCreate,Unregisters,SnapshotGets;
static UINT64 TimeUs;
static EFI_TPL Tpl;
static EFI_STATUS LocateStatus,RegisterStatus,UnregisterStatus,InstallStatus,RemoveStatus,ArmStatus,CancelStatus,CloseStatus;
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memcpy(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
UINT64 EFIAPI GetPerformanceCounter(VOID){return TimeUs;}
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 T){return T*1000;}
static UINT32 Get(CONST UINT8 *P){return P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static VOID Put(UINT8 *P,UINT32 V){P[0]=(UINT8)V;P[1]=(UINT8)(V>>8);P[2]=(UINT8)(V>>16);P[3]=(UINT8)(V>>24);}
static EFI_TPL EFIAPI Raise(EFI_TPL N){EFI_TPL Old=Tpl;assert(N>=Old);Tpl=N;return Old;}
static VOID EFIAPI Restore(EFI_TPL Old){Tpl=Old;}
static VOID Link(BOOLEAN Online,UINT32 A,UINT32 P){
 LinkState.Online=Online;LinkState.AdspGeneration=A;LinkState.PeerGeneration=P;
 LinkState.LastError=Online?EFI_SUCCESS:EFI_NOT_READY;
 if(Registered){EFI_TPL Old=Raise(TPL_NOTIFY);Changed(NULL,&LinkState);Restore(Old);}
}
static EFI_STATUS EFIAPI Bind(EFI_PMIC_GLINK_CR_PROTOCOL *This,UINT32 Owner,VOID *Identity,
 EFI_PMIC_GLINK_RECEIVE ReceiveFn,EFI_PMIC_GLINK_STATE_CHANGE StateFn,VOID *Context){
 assert(This==&Provider&&Owner==32778&&Identity==&mBattery&&Context==NULL&&TimerOpen&&ExitOpen&&!Registered);
 if(RegisterStatus!=EFI_SUCCESS)return RegisterStatus;
 Rx=ReceiveFn;Changed=StateFn;Client=Identity;Registered=TRUE;
 EFI_TPL Old=Raise(TPL_NOTIFY);Changed(Context,&LinkState);Restore(Old);
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Unbind(EFI_PMIC_GLINK_CR_PROTOCOL *This,UINT32 Owner,VOID *Identity){
 assert(This==&Provider&&Owner==32778&&Identity==Client&&Registered&&mStopped&&!mBattery.State.PeerReady);
 ++Unregisters;if(UnregisterStatus!=EFI_SUCCESS)return UnregisterStatus;
 Registered=FALSE;Rx=NULL;Changed=NULL;Client=NULL;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Packet(EFI_PMIC_GLINK_CR_PROTOCOL *This,UINT32 Owner,VOID *Identity,CONST VOID *Data,UINTN N){
 CONST UINT8 *P=Data;UINT8 Reply[24]={0};
 assert(This==&Provider&&Owner==32778&&Identity==Client&&Registered&&!mStopped&&Tpl==TPL_NOTIFY&&N==24&&Get(P)==32778);
 ++Sends;
 if(!LinkState.Online)return EFI_NOT_READY;
 if(ImmediateReply){
  Put(Reply,32778);Put(Reply+4,1);Put(Reply+8,Get(P+8));Put(Reply+12,Get(P+16));
  UINT32 Value=1;
  if(Get(P+8)==0x30&&Get(P+16)==4)Value=6799;
  if(Get(P+8)==0x30&&Get(P+16)==7)Value=4123456;
  if(Get(P+8)==0x30&&Get(P+16)==9)Value=(UINT32)-12345;
  if(Get(P+8)==0x30&&Get(P+16)==12)Value=2850;
  Put(Reply+16,Value);Rx(NULL,Reply,24);
 }
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ProviderState(EFI_PMIC_GLINK_CR_PROTOCOL *This,EFI_PMIC_GLINK_CR_STATE *Out){
 assert(This==&Provider);++SnapshotGets;*Out=LinkState;return Out->Online?EFI_SUCCESS:EFI_NOT_READY;
}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **Out){
 assert(G==&gEfiPmicGlinkCrProtocolGuid&&R==NULL);if(LocateStatus!=EFI_SUCCESS)return LocateStatus;*Out=&Provider;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Event(UINT32 Type,EFI_TPL Level,EFI_EVENT_NOTIFY Fn,VOID *C,EFI_EVENT *Out){
 assert(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL)&&Level==TPL_NOTIFY&&C==NULL);
 if(++Creates==FailCreate)return EFI_OUT_OF_RESOURCES;
 TimerOpen=TRUE;TimerFn=Fn;*Out=(EFI_EVENT)1;
 EFI_TPL Old=Raise(TPL_NOTIFY);Fn(*Out,C);Restore(Old);assert(Sends==0);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI EventEx(UINT32 Type,EFI_TPL Level,EFI_EVENT_NOTIFY Fn,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *Out){
 assert(Type==EVT_NOTIFY_SIGNAL&&Level==TPL_NOTIFY&&C==NULL&&G==&gEfiEventExitBootServicesGuid);
 if(++Creates==FailCreate)return EFI_OUT_OF_RESOURCES;
 ExitOpen=TRUE;ExitFn=Fn;*Out=(EFI_EVENT)2;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY Type,UINT64 Delay){
 assert(E==(EFI_EVENT)1&&TimerOpen);
 if(Type==TimerCancel){assert(mStopped&&Delay==0);return CancelStatus;}
 assert(Type==TimerPeriodic&&Delay==1000000&&Published&&Registered&&ExitOpen);return ArmStatus;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){
 assert(mStopped&&!Registered);
 if(CloseStatus!=EFI_SUCCESS)return CloseStatus;
 if(E==(EFI_EVENT)1){assert(TimerOpen);TimerOpen=FALSE;}else{assert(E==(EFI_EVENT)2&&ExitOpen);ExitOpen=FALSE;}
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
 va_list A;assert(*H==(EFI_HANDLE)0x1234&&TimerOpen&&ExitOpen&&Registered);
 va_start(A,H);assert(va_arg(A,EFI_GUID*)==&gEfiBatteryChargerCrProtocolGuid);
 assert(va_arg(A,VOID*)==&mProtocol&&va_arg(A,VOID*)==NULL);va_end(A);
 Published=InstallStatus==EFI_SUCCESS;return InstallStatus;
}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE H,...){
 va_list A;assert(H==(EFI_HANDLE)0x1234&&Published&&mStopped&&!Registered);
 va_start(A,H);assert(va_arg(A,EFI_GUID*)==&gEfiBatteryChargerCrProtocolGuid);
 assert(va_arg(A,VOID*)==&mProtocol&&va_arg(A,VOID*)==NULL);va_end(A);
 if(RemoveStatus==EFI_SUCCESS)Published=FALSE;return RemoveStatus;
}
static VOID Reset(VOID){
 memset(&mBattery,0,sizeof(mBattery));memset(&mProtocol,0,sizeof(mProtocol));mTransport=NULL;mTimer=mExit=NULL;
 mStopped=TRUE;mRegistered=mPublished=FALSE;mLoggedTimeouts=mLoggedMalformed=0;
 TimerOpen=ExitOpen=Published=Registered=OnlineAtRegister=ImmediateReply=FALSE;
 Sends=Creates=FailCreate=Unregisters=SnapshotGets=0;TimeUs=0;Tpl=TPL_APPLICATION;
 LocateStatus=RegisterStatus=UnregisterStatus=InstallStatus=RemoveStatus=ArmStatus=CancelStatus=CloseStatus=EFI_SUCCESS;
 memset(&Provider,0,sizeof(Provider));Provider.Revision=EFI_PMIC_GLINK_CR_PROTOCOL_REVISION;
 Provider.Register=Bind;Provider.Unregister=Unbind;Provider.Send=Packet;Provider.GetState=ProviderState;
 Services.LocateProtocol=Locate;Services.CreateEvent=Event;Services.CreateEventEx=EventEx;
 Services.SetTimer=Timer;Services.CloseEvent=Close;Services.InstallMultipleProtocolInterfaces=Install;
 Services.UninstallMultipleProtocolInterfaces=Remove;Services.RaiseTPL=Raise;Services.RestoreTPL=Restore;
 Link(FALSE,1,2);
}
static VOID Poll(VOID){EFI_TPL Old=Raise(TPL_NOTIFY);TimerFn((EFI_EVENT)1,NULL);Restore(Old);}
static VOID EpochRecovery(VOID){
 for(UINT32 Mode=0;Mode<4;++Mode){
  Reset();Link(TRUE,7,9);assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  Poll();assert(Sends==1&&mBattery.State.Pending&&mBattery.PendingIndex==0);
  if(Mode==0)Link(TRUE,7,10);
  else Link(Mode==3,Mode==1?7:Mode==2?0:6,10);
  assert(mBattery.AdspGeneration==7&&!mBattery.State.Pending&&mBattery.AwaitLateMask==1&&!mBattery.State.ValidMask);
  if(Mode){Poll();assert(Sends==1);Link(TRUE,7,11);}
  TimeUs=100;Poll();assert(Sends==2&&mBattery.PendingIndex!=0&&(mBattery.AwaitLateMask&1));
  UINT8 Original[24]={0};Put(Original,32778);Put(Original+4,1);Put(Original+8,0x30);Put(Original+12,2);Put(Original+16,1);
  EFI_TPL Old=Raise(TPL_NOTIFY);Rx(NULL,Original,sizeof(Original));Restore(Old);
  assert(!(mBattery.AwaitLateMask&1)&&!(mBattery.State.ValidMask&CR_BATTERY_VALID_PRESENT));
  assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
 }
}
int main(void){
 EpochRecovery();
 CR_BATTERY_CHARGER_SNAPSHOT S;
 Reset();assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
 for(UINT32 I=0;I<20;++I){TimeUs+=100000;Poll();}
 assert(Sends==0&&Snapshot(&mProtocol,&S)==EFI_NOT_READY&&S.ValidMask==0);
 assert(SnapshotGets==0); /* UI snapshot does not RPC/discover or call transport GetState. */
 Link(TRUE,1,3);ImmediateReply=TRUE;
 for(UINT32 I=0;I<7;++I){TimeUs+=100000;Poll();}
 assert(Snapshot(&mProtocol,&S)==EFI_SUCCESS&&S.ValidMask==127&&S.SocHundredthsPercent==6799);
 assert(S.CurrentUa==-12345&&S.TemperatureMilliC==28500&&Sends==7);
 Link(FALSE,1,4);assert(Snapshot(&mProtocol,&S)==EFI_NOT_READY&&S.ValidMask==0);
 TimeUs+=100000;Poll();assert(Sends==7);
 Link(TRUE,2,5);assert(Snapshot(&mProtocol,&S)==EFI_NOT_READY);
 TimeUs+=100000;Poll();assert(Sends==8);
 EFI_TPL Old=Raise(TPL_NOTIFY);ExitFn((EFI_EVENT)2,NULL);Restore(Old);
 assert(mStopped&&!Registered&&!mBattery.State.ValidMask&&TimerOpen&&ExitOpen);
 Poll();assert(Sends==8&&BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!TimerOpen&&!ExitOpen&&!Published);
 for(UINT32 F=1;F<=2;++F){Reset();FailCreate=F;assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_OUT_OF_RESOURCES&&!TimerOpen&&!ExitOpen&&!Registered&&!Published);}
 Reset();RegisterStatus=EFI_NOT_READY;assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_NOT_READY&&!TimerOpen&&!ExitOpen&&!Registered);
 Reset();InstallStatus=EFI_ACCESS_DENIED;assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_ACCESS_DENIED&&!Registered&&!TimerOpen&&!ExitOpen);
 Reset();InstallStatus=EFI_ACCESS_DENIED;UnregisterStatus=EFI_NOT_READY;
 assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS&&mStopped&&mRegistered&&TimerOpen&&ExitOpen&&!Published);
 Poll();assert(Sends==0);assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_ALREADY_STARTED);
 UnregisterStatus=EFI_SUCCESS;assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!Registered&&!TimerOpen);
 Reset();ArmStatus=EFI_DEVICE_ERROR;assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_DEVICE_ERROR&&!Registered&&!Published&&!TimerOpen&&!ExitOpen);
 Reset();ArmStatus=EFI_DEVICE_ERROR;RemoveStatus=EFI_ACCESS_DENIED;
 assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS&&mStopped&&Published&&!Registered&&TimerOpen&&ExitOpen);
 assert(Snapshot(&mProtocol,&S)==EFI_NOT_READY);RemoveStatus=EFI_SUCCESS;
 assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&!Published&&!TimerOpen);
 Reset();assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);CancelStatus=EFI_DEVICE_ERROR;
 assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR&&Registered&&Published&&TimerOpen&&ExitOpen&&mStopped);
 Poll();assert(Sends==0);CancelStatus=EFI_SUCCESS;assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
 Reset();assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);CloseStatus=EFI_DEVICE_ERROR;
 assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR&&!Registered&&!Published&&TimerOpen&&ExitOpen);
 UINT32 U=Unregisters;CloseStatus=EFI_SUCCESS;
 assert(BatteryChargerCrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS&&Unregisters==U&&!TimerOpen&&!ExitOpen);
 Reset();Provider.Revision=0;assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_UNSUPPORTED&&Creates==0);
 Reset();LocateStatus=EFI_NOT_FOUND;assert(BatteryChargerCrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_NOT_FOUND&&Creates==0);
 puts("Battery DXE actual core+driver: offline/async7 replies/pure snapshots/generation/EBS invalidation, partial event/publication/timer/subscriber cleanup retention+retry PASS");
 return 0;
}
