/* Actual PM8150B native adapter and core; firmware/provider callback faults.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#ifndef CR_PMIC_CC_DXE_SOURCE
#define CR_PMIC_CC_DXE_SOURCE "../Driver/PmicCcCrDxe/PmicCcCrDxe.c"
#endif
#include CR_PMIC_CC_DXE_SOURCE
EFI_GUID gEfiCrSpmiProtocolGuid,gEfiUsbTypeCCrProtocolGuid,gEfiEventExitBootServicesGuid;
static EFI_BOOT_SERVICES Bs;EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_CR_SPMI_PROTOCOL Provider;
static UINT8 Raw[8];
static UINT32 Reads,Subscribes,Enables,Unsubscribes,Acquires,Releases,Installs,Closes,Signals,Timers,Guards;
static UINT32 ExitInside;static BOOLEAN Reenter,UnloadInside,SignalFail,TimerFail,SubscribeFail,UnsubscribeFail,ReleaseFail;
static UINT32 GuardDepth;
static CR_INTERRUPT_HANDLER Handler;static VOID *HandlerContext;
static VOID Trigger(UINT32 Phase){if(ExitInside==Phase){ExitInside=0;Exit(NULL,NULL);}}
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *A,UINT32 Old,UINT32 New){return __sync_val_compare_and_swap(A,Old,New);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *A,CONST VOID *B,UINTN N){return memcpy(A,B,N);}
VOID *EFIAPI SetMem(VOID *A,UINTN N,UINT8 V){return memset(A,V,N);}
CR_STATUS CrInterruptGuardAcquire(CR_INTERRUPT_GUARD *G){assert(gBS);++Guards;++GuardDepth;G->Active=TRUE;return CR_SUCCESS;}
VOID CrInterruptGuardRelease(CR_INTERRUPT_GUARD *G){assert(G->Active&&GuardDepth);G->Active=FALSE;--GuardDepth;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
static CR_STATUS Check(VOID *X){assert(X==&mBus&&mClient.Provider);return CR_SUCCESS;}
static CR_STATUS Read(VOID *X,UINT8 S,UINT16 A,UINT8 *D,UINTN N){
 assert(GuardDepth&&X==&mBus&&S==2);++Reads;
 if(A==0x100){assert(N==6);UINT8 R[]={0,0,0,2,0x51,0x20};memcpy(D,R,6);Trigger(3);}
 else if(A==0x1506){assert(N==6);memcpy(D,Raw,6);Trigger(6);}
 else if(A==0x1740){assert(N==1);*D=Raw[6];}else{assert(A==0x1746&&N==1);*D=Raw[7];}return CR_SUCCESS;
}
static CR_STATUS Subscribe(VOID *X,UINT8 S,UINT8 P,UINT8 B,CR_INTERRUPT_TRIGGER_TYPE T,CR_INTERRUPT_HANDLER H,VOID *C,CR_SPMI_IRQ *Token){
 assert(GuardDepth&&X==&mBus&&S==2&&P==0x15&&B<8);(VOID)T;++Subscribes;
 Token->Provider=&Provider;Token->Generation=B+1;Handler=H;HandlerContext=C;Trigger(4);
 return SubscribeFail?CR_DEVICE_ERROR:CR_SUCCESS;
}
static CR_STATUS Enable(VOID *X,CR_SPMI_IRQ *T,BOOLEAN On){assert(GuardDepth&&X==&mBus&&T->Provider&&On);++Enables;Trigger(5);return CR_SUCCESS;}
static CR_STATUS Unsubscribe(VOID *X,CR_SPMI_IRQ *T){
 assert(GuardDepth&&X==&mBus&&T->Provider);++Unsubscribes;Trigger(10);
 if(UnsubscribeFail)return CR_DEVICE_ERROR;memset(T,0,sizeof *T);return CR_SUCCESS;
}
static EFI_STATUS EFIAPI Acquire(EFI_CR_SPMI_PROTOCOL *P,EFI_CR_SPMI_ROLE R,VOID *Owner,CR_SPMI_CLIENT *T,CR_SPMI_BUS *B){
 assert(GuardDepth&&P==&Provider&&R==CrSpmiClientTypeC&&Owner==&mPort&&T==&mClient&&B==&mBus);++Acquires;
 if(UnloadInside){UnloadInside=FALSE;assert(PmicCcCrUnload(NULL)==EFI_ACCESS_DENIED);}
 *T=(CR_SPMI_CLIENT){P,Owner,1};*B=(CR_SPMI_BUS){.Check=Check,.Read=Read,.Subscribe=Subscribe,.Enable=Enable,.Unsubscribe=Unsubscribe};Trigger(2);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Release(EFI_CR_SPMI_PROTOCOL *P,CR_SPMI_CLIENT *T){assert(GuardDepth&&P==&Provider&&T==&mClient);++Releases;if(ReleaseFail)return EFI_DEVICE_ERROR;memset(T,0,sizeof *T);Trigger(11);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Target(EFI_CR_SPMI_PROTOCOL *P,CR_PMIC_CC_TARGET *T){assert(P==&Provider);*T=(CR_PMIC_CC_TARGET){8245,TRUE};return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *R,VOID **P){
 (VOID)R;if(Reenter){Reenter=FALSE;assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);}
 if(G==&gEfiCrSpmiProtocolGuid){*P=&Provider;return EFI_SUCCESS;}assert(G==&gEfiUsbTypeCCrProtocolGuid);return EFI_NOT_FOUND;
}
static EFI_STATUS EFIAPI Create(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,VOID *C,EFI_EVENT *E){
 assert(P==TPL_CALLBACK&&N==Service&&!C);*E=(VOID *)(UINTN)(T==EVT_NOTIFY_SIGNAL?1:2);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateEx(UINT32 T,EFI_TPL P,EFI_EVENT_NOTIFY N,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){
 assert(T==EVT_NOTIFY_SIGNAL&&P==TPL_NOTIFY&&N==Exit&&!C&&G==&gEfiEventExitBootServicesGuid);*E=(VOID *)3;Trigger(1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Signal(EFI_EVENT E){assert(E==mWork);++Signals;return SignalFail?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY T,UINT64 N){assert(E==mTimer&&(T==TimerCancel||T==TimerRelative));(VOID)N;++Timers;Trigger(8);return TimerFail?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E);++Closes;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
 va_list A;va_start(A,H);assert(va_arg(A,VOID *)==&gEfiUsbTypeCCrProtocolGuid&&va_arg(A,VOID *)==&mProtocol&&va_arg(A,VOID *)==NULL);va_end(A);*H=(VOID *)4;++Installs;Trigger(7);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Uninstall(EFI_HANDLE H,...){assert(H==(VOID *)4);return EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL T){assert(T==TPL_HIGH_LEVEL);return TPL_APPLICATION;}
static VOID EFIAPI Restore(EFI_TPL T){assert(T==TPL_APPLICATION);}
static VOID Fresh(VOID){
 memset(&mPort,0,sizeof mPort);memset(&mClient,0,sizeof mClient);memset(&mBus,0,sizeof mBus);
 mProvider=NULL;mHandle=NULL;mWork=mTimer=mExit=NULL;mStopped=mQueued=mRunning=mPublished=FALSE;
#ifndef CR_PMIC_CC_OLD_DXE
 mEntered=mEntryBusy=mExiting=mStopping=FALSE;
#endif
 mRetry=0;memset(Raw,0,sizeof Raw);Raw[2]=8;Raw[3]=Raw[4]=0x20;Raw[5]=0x69;
 Reads=Subscribes=Enables=Unsubscribes=Acquires=Releases=Installs=Closes=Signals=Timers=Guards=GuardDepth=ExitInside=0;
 Reenter=UnloadInside=SignalFail=TimerFail=SubscribeFail=UnsubscribeFail=ReleaseFail=FALSE;
 Provider=(EFI_CR_SPMI_PROTOCOL){1,sizeof Provider,Acquire,Release,Target};
 Bs=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.CreateEvent=Create,.CreateEventEx=CreateEx,.SignalEvent=Signal,.SetTimer=Timer,.CloseEvent=Close,.InstallMultipleProtocolInterfaces=Install,.UninstallMultipleProtocolInterfaces=Uninstall};gBS=&Bs;
}
static VOID CheckClosed(VOID){
 CR_TYPEC_NON_PD_HOST_WITNESS W;EFI_USB_TYPEC_STATE State;UINT32 G=Guards,R=Reads;
 gBS=NULL;assert(Witness(&mProtocol,&W)==EFI_NOT_READY&&Get(&mProtocol,&State)==EFI_NOT_READY);Service(mWork,NULL);Wake(NULL);gBS=&Bs;
 assert(Guards==G&&Reads==R&&!mPort.WitnessFlags);
}
int main(int Count,char **Args){
 if(Count>1&&!strcmp(Args[1],"entry")){Fresh();Reenter=TRUE;assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&Installs==1);return 0;}
 if(Count>1&&!strcmp(Args[1],"unload")){Fresh();UnloadInside=TRUE;assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&Installs==1);return 0;}
 if(Count>1&&!strcmp(Args[1],"exit")){
  for(UINT32 P=1;P<=7;P++){
   Fresh();ExitInside=P;assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&mStopped&&!Releases&&!Unsubscribes&&!Closes);
   assert(Installs==(P==7?1U:0U));assert(Acquires==(P==1?0U:1U));
   if(P==3)assert(!Subscribes);if(P==4)assert(Subscribes==1&&!Enables);if(P==5)assert(Enables==1);if(P==6)assert(Reads==2);
   CheckClosed();assert(PmicCcCrUnload(NULL)==EFI_ACCESS_DENIED&&PmicCcCrEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);
  }return 0;
 }
 Fresh();assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&Installs==1&&Subscribes==8&&Enables==8);
 CR_TYPEC_NON_PD_HOST_WITNESS W;UINT32 Before=Reads,G=Guards;gBS=NULL;assert(Witness(&mProtocol,&W)==EFI_SUCCESS&&Reads==Before&&Guards==G);gBS=&Bs;
 assert((W.Flags&127)==127&&!mPort.State.PdReady);
 TimerFail=TRUE;Service(mTimer,NULL);assert(Witness(&mProtocol,&W)==EFI_NOT_READY&&!mPort.WitnessFlags);TimerFail=FALSE;
 Handler(HandlerContext);Service(mWork,NULL);assert(Witness(&mProtocol,&W)==EFI_SUCCESS);
 SignalFail=TRUE;Wake(NULL);assert(Witness(&mProtocol,&W)==EFI_NOT_READY&&!mPort.WitnessFlags);SignalFail=FALSE;
 Service(mWork,NULL);assert(Witness(&mProtocol,&W)==EFI_NOT_READY);Handler(HandlerContext);Service(mWork,NULL);assert(Witness(&mProtocol,&W)==EFI_SUCCESS);
 mRunning=TRUE;assert(Stop()==EFI_NOT_READY&&!mPort.WitnessFlags);mRunning=FALSE;assert(Stop()==EFI_SUCCESS&&!mClient.Provider);CheckClosed();
 Fresh();SubscribeFail=UnsubscribeFail=TRUE;assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&mClient.Provider&&!Installs&&!mPort.WitnessFlags);
 UnsubscribeFail=FALSE;assert(PmicCcCrUnload(NULL)==EFI_SUCCESS&&!mClient.Provider);
 Fresh();SubscribeFail=ReleaseFail=TRUE;assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&mClient.Provider&&!Installs);ReleaseFail=FALSE;assert(PmicCcCrUnload(NULL)==EFI_SUCCESS);
 for(UINT32 P=8;P<=11;P++){
  if(P==9)continue;Fresh();assert(PmicCcCrEntryPoint(NULL,NULL)==EFI_SUCCESS);ExitInside=P;
  if(P==8)Service(mTimer,NULL);else assert(PmicCcCrUnload(NULL)==EFI_ACCESS_DENIED);
  CheckClosed();assert(!Closes);if(P==10)assert(Unsubscribes==1&&!Releases);
 }
 puts("PM8150B actual DXE/core: pure atomic witness, transport/stop invalidation, reentry/unload exclusion, EBS callback closure and retained exact partial claims PASS");return 0;
}
