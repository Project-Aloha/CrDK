/* Actual native resource + OSKAL + SDHCI engines, fallible provider model. MIT */
#include <Uefi.h>
#include "SdTargetTestProvider.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
#include <Protocol/HardwareInterrupt2.h>
#include "../Driver/SdhciCrDxe/SdResources.c"
#include "../Library/OskInterruptLib/oskinterrupt.c"
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiClockCrProtocolGuid=EFI_CLOCK_CR_PROTOCOL_GUID;
EFI_GUID gEfiInterconnectCrProtocolGuid=EFI_INTERCONNECT_CR_PROTOCOL_GUID;
EFI_GUID gEfiRpmhCrProtocolGuid=EFI_RPMH_CR_PROTOCOL_GUID;
EFI_GUID gEfiGpioCrProtocolGuid=EFI_GPIO_CR_PROTOCOL_GUID;
EFI_GUID gHardwareInterrupt2ProtocolGuid=EFI_HARDWARE_INTERRUPT2_PROTOCOL_GUID;
STATIC CR_SDHC Core;
STATIC UINT8 Regs[0x1000];
STATIC EFI_TPL Tpl;
STATIC UINT32 ClockRefs[2],RailRefs[2],PathRefs[2],Requests,Actions,Reads,Writes,FullReset,Acks,Irqs,Closes;
STATIC UINT32 AcquireClockFail,OffClockFail,RailFail,PathFail,ResetFail,CloseFail,ClearFail,AckFail,UnregisterFail;
STATIC UINT32 AcquireNumber,OffNumber,RailNumber,SetPathNumber;
STATIC UINT64 Delays,CauseAt;
STATIC BOOLEAN DropCause,Card,PadOwned,PadActive,Queued,IrqInstalled,FailConnect,CommitConnect,InIrq;
STATIC BOOLEAN InjectClockCause;
STATIC BOOLEAN PowerHandshake;
STATIC UINT32 DeferredCause,ResetCompletionReads;
STATIC EFI_STATUS HostControlStatus;
STATIC BOOLEAN DropIoCause;
STATIC EFI_STATUS FloorStatus,CardStatus;
STATIC UINT32 FloorCalls;
STATIC EFI_EVENT_NOTIFY Work;
STATIC HARDWARE_INTERRUPT_HANDLER HwHandler;
STATIC EFI_BOOT_SERVICES Boot;
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"assert %s:%zu %s\n",F,(size_t)L,D);abort();}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return TRUE;}
BOOLEAN EFIAPI DebugClearMemoryEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return TRUE;}
STATIC UINT32 R(UINT32 O,UINT32 W){UINT32 V=0;memcpy(&V,Regs+O,W);return V;}
STATIC VOID V(UINT32 O,UINT32 W,UINT32 X){memcpy(Regs+O,&X,W);}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL N){EFI_TPL Old=Tpl;assert(N>=Old);Tpl=N;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL N){assert(N<=Tpl);Tpl=N;
  if(Queued&&Tpl<TPL_CALLBACK){Queued=FALSE;EFI_TPL Old=Tpl;Tpl=TPL_CALLBACK;Work((VOID *)1,NULL);Tpl=Old;}}
STATIC VOID Fire(VOID){if(!HwHandler||!(R(0x244,4)&R(0x240,4)))return;
  ++Irqs;InIrq=TRUE;HwHandler(255,(EFI_SYSTEM_CONTEXT){0});InIrq=FALSE;}
UINTN EFIAPI MicroSecondDelay(UINTN N){Delays+=N;if(CauseAt&&Delays>=CauseAt){
  CauseAt=0;V(0x240,4,DeferredCause?DeferredCause:(R(0x29,1)?CR_SDHC_POWER_ON:CR_SDHC_POWER_OFF));DeferredCause=0;Fire();}return N;}
STATIC CR_STATUS Ready(VOID *C){assert(C==&Core);return CrSdResourcesReady();}
STATIC CR_STATUS Read(VOID *C,UINT32 O,UINT8 W,UINT32 *X){assert(C==&Core&&O+W<=sizeof(Regs));++Reads;if(O==0x2f&&ResetCompletionReads&&!--ResetCompletionReads){V(0x2f,1,0);V(0x29,1,0);V(0x2c,2,0);}
  *X=R(O,W);return CR_SUCCESS;}
STATIC CR_STATUS Write(VOID *C,UINT32 O,UINT8 W,UINT32 X){assert(C==&Core&&O+W<=sizeof(Regs));++Writes;
  if(O==0x248){if(ClearFail)return CR_DEVICE_ERROR;V(0x240,4,R(0x240,4)&~X);return CR_SUCCESS;}
  if(O==0x24c){++Acks;if(AckFail){--AckFail;return CR_DEVICE_ERROR;}
    if((X&1)&&R(0x2f,1))ResetCompletionReads=3;}
  if(O==0x2f){if(X==1)++FullReset;if(ResetFail)return CR_DEVICE_ERROR;
    if(X==1&&(R(0x29,1)&1)){
      V(O,1,X);DeferredCause=CR_SDHC_POWER_OFF;CauseAt=Delays+100;return CR_SUCCESS;}
    V(O,1,0);V(0x29,1,0);V(0x2c,2,0);return CR_SUCCESS;}
  if(O==0x3e&&PowerHandshake){if(HostControlStatus!=EFI_SUCCESS)return HostControlStatus;
    if(!DropIoCause){DeferredCause=CR_SDHC_IO_HIGH;CauseAt=Delays+100;}}
  if(O==0x29){++Requests;if(!DropCause)CauseAt=Delays+100;}
  if(O==0x2c&&(X&1))X|=2;
  V(O,W,X);return CR_SUCCESS;}
STATIC CR_STATUS Stall(VOID *C,UINT32 N){assert(C==&Core);MicroSecondDelay(N);return CR_SUCCESS;}
STATIC CR_STATUS Barrier(VOID *C){assert(C==&Core);return CR_SUCCESS;}
STATIC VOID Provider(VOID){assert(Tpl<=TPL_CALLBACK&&!InIrq);++Actions;}
STATIC UINT32 ClockIndex(CONST CHAR8 *N){if(!strcmp(N,"gcc_sdcc2_ahb_clk"))return 0;assert(!strcmp(N,"gcc_sdcc2_apps_clk"));return 1;}
STATIC EFI_STATUS EFIAPI AcquireClock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 H,BOOLEAN *Owned){
  (VOID)P;assert(!strcmp(C,"gcc"));Provider();UINT32 I=ClockIndex(N);
  assert(!ClockRefs[I]);assert(I?H==400000||H==25000000||H==50000000:H==0);
  ++ClockRefs[I];*Owned=TRUE;return ++AcquireNumber==AcquireClockFail?EFI_TIMEOUT:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI SetClock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 H,BOOLEAN On){
  (VOID)P;(VOID)H;assert(!strcmp(C,"gcc")&&!On);Provider();UINT32 I=ClockIndex(N);assert(ClockRefs[I]==1);
  if(I&&InjectClockCause){InjectClockCause=FALSE;assert(!HwHandler&&!R(0x244,4));
    V(0x240,4,CR_SDHC_POWER_ON);UINT32 Before=Irqs;Fire();assert(Irqs==Before);}
  if(++OffNumber==OffClockFail)return EFI_TIMEOUT;--ClockRefs[I];return EFI_SUCCESS;}
STATIC UINT32 RailIndex(CONST CHAR8 *N){if(!strcmp(N,"ldoc9"))return 0;assert(!strcmp(N,"ldoc6"));return 1;}
STATIC EFI_STATUS EFIAPI AcquireRail(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *N,UINT32 M,UINT8 Mode,BOOLEAN *Owned){
  (VOID)P;Provider();assert(M==3008&&Mode==7);UINT32 I=RailIndex(N);assert(!RailRefs[I]);RailRefs[I]=1;*Owned=TRUE;
  return ++RailNumber==RailFail?EFI_TIMEOUT:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI RailOffModel(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *N,BOOLEAN On){
  (VOID)P;assert(!On);Provider();UINT32 I=RailIndex(N);assert(RailRefs[I]);--RailRefs[I];return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Floor(EFI_RPMH_CR_PROTOCOL *P){(VOID)P;Provider();++FloorCalls;return FloorStatus;}
STATIC EFI_STATUS EFIAPI AcquirePath(EFI_INTERCONNECT_CR_PROTOCOL *P,CONST CHAR8 *C,UINT32 S,UINT32 D,INTERCONNECT_PATH_HANDLE *H){
  (VOID)P;Provider();assert(!strcmp(C,"qcom,sm8450-sdhci")&&(S==0x8000||S==0x8002)&&D==S+1);
  UINT32 I=(S-0x8000)/2;assert(!PathRefs[I]);PathRefs[I]=1;*H=I+1;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE H,UINT64 A,UINT64 Peak){
  (VOID)P;Provider();UINTN I=(UINTN)H-1;assert(I<2&&PathRefs[I]&&A==mSdTarget->Paths[I].Average&&Peak==mSdTarget->Paths[I].Peak);
  return ++SetPathNumber==PathFail?EFI_TIMEOUT:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI ReleasePath(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE H){
  (VOID)P;Provider();UINTN I=(UINTN)H-1;assert(I<2&&PathRefs[I]);PathRefs[I]=0;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI AcquirePads(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,BOOLEAN *Owned){(VOID)P;Provider();assert(Owner==&Core&&!PadOwned);*Owned=PadOwned=TRUE;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI SetPads(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,BOOLEAN On){(VOID)P;Provider();assert(Owner==&Core&&PadOwned);PadActive=On;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI ReadCd(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,BOOLEAN *High){(VOID)P;Provider();assert(Owner==&Core&&PadOwned);*High=Card;return CardStatus;}
STATIC EFI_STATUS EFIAPI ReleasePads(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner){(VOID)P;Provider();assert(Owner==&Core&&PadOwned&&mHostStopped);PadOwned=PadActive=FALSE;return EFI_SUCCESS;}
STATIC EFI_CLOCK_CR_PROTOCOL Clock={.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.SetClock=SetClock,.AcquireClock=AcquireClock};
STATIC EFI_RPMH_CR_PROTOCOL Rail={.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.AcquireVreg=AcquireRail,.RpmhEnableVreg=RailOffModel,.EnsureBootFloor=Floor};
STATIC EFI_INTERCONNECT_CR_PROTOCOL Icb={.Revision=EFI_INTERCONNECT_CR_PROTOCOL_REVISION,.AcquirePath=AcquirePath,.SetBandwidth=Bandwidth,.ReleasePath=ReleasePath};
STATIC EFI_GPIO_CR_PROTOCOL Gpio={.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.AcquireSd=AcquirePads,.SetSdActive=SetPads,.ReadSdCardDetect=ReadCd,.ReleaseSd=ReleasePads};
STATIC EFI_STATUS EFIAPI RegisterIrq(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S,HARDWARE_INTERRUPT_HANDLER H){
  (VOID)P;assert(S==255&&Tpl==TPL_HIGH_LEVEL);
  if(!H){if(UnregisterFail)return EFI_DEVICE_ERROR;HwHandler=NULL;IrqInstalled=FALSE;return EFI_SUCCESS;}
  assert(!HwHandler);if(!FailConnect||CommitConnect){HwHandler=H;IrqInstalled=TRUE;}
  return FailConnect?EFI_DEVICE_ERROR:EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI IrqOp(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S){(VOID)P;assert(S==255&&Tpl==TPL_HIGH_LEVEL);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Trigger(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S,EFI_HARDWARE_INTERRUPT2_TRIGGER_TYPE T){
  (VOID)P;assert(S==255&&T==EFI_HARDWARE_INTERRUPT2_TRIGGER_LEVEL_HIGH&&Tpl==TPL_HIGH_LEVEL);return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Eoi(EFI_HARDWARE_INTERRUPT2_PROTOCOL *P,HARDWARE_INTERRUPT_SOURCE S){InIrq=FALSE;return IrqOp(P,S);}
STATIC EFI_HARDWARE_INTERRUPT2_PROTOCOL Hw={.RegisterInterruptSource=RegisterIrq,.EnableInterruptSource=IrqOp,
  .DisableInterruptSource=IrqOp,.EndOfInterrupt=Eoi,.SetTriggerType=Trigger};
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *K,VOID **Out){assert(!K&&Tpl<=TPL_NOTIFY);
  if(G==&gEfiClockCrProtocolGuid)*Out=&Clock;else if(G==&gEfiRpmhCrProtocolGuid)*Out=&Rail;
  else if(G==&gEfiInterconnectCrProtocolGuid)*Out=&Icb;else if(G==&gEfiGpioCrProtocolGuid)*Out=&Gpio;
  else {assert(G==&gHardwareInterrupt2ProtocolGuid);*Out=&Hw;}return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL Priority,EFI_EVENT_NOTIFY Fn,VOID *C,EFI_EVENT *Out){
  assert(Type==EVT_NOTIFY_SIGNAL&&Priority==TPL_CALLBACK&&!C&&Tpl<=TPL_CALLBACK);Work=Fn;*Out=(VOID *)1;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Signal(EFI_EVENT E){assert(E==(VOID *)1&&Tpl==TPL_HIGH_LEVEL);Queued=TRUE;return EFI_SUCCESS;}
STATIC EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E==(VOID *)1&&!IrqInstalled);++Closes;if(CloseFail){--CloseFail;return EFI_DEVICE_ERROR;}Queued=FALSE;Work=NULL;return EFI_SUCCESS;}
STATIC VOID New(VOID){
  memset(Regs,0,sizeof(Regs));memset(mCrIntTable,0,sizeof(mCrIntTable));mHwInterrupt2=NULL;mDispatchDepth=0;
  ResetCompletionReads=0;HostControlStatus=EFI_SUCCESS;DropIoCause=FALSE;
  mIoValid=mIoHigh=mInternalSync=FALSE;mIoSequence=mSyncBefore=0;mSyncRequest=0;mSyncPrior=FALSE;mIoStatus=EFI_NOT_READY;
  mClock=NULL;mIcb=NULL;mRail=NULL;mGpio=NULL;mController=NULL;mPowerWork=NULL;
  mTestSdTarget=NULL;mTestSdMissing=FALSE;
  mGpioOwned=mAccess=mBusy=mClosing=mFrozen=mIrqOwned=mHostMayLive=mHostStopped=mIrqPaused=FALSE;
  mRequestActive=mBusValid=mBusOn=FALSE;mRate=0;mPowerStatus=EFI_SUCCESS;mBusSequence=0;mBusStatus=EFI_NOT_READY;
  memset(mPaths,0,sizeof(mPaths));for(UINT32 I=0;I<2;++I){mClocks[I].Owned=mRails[I].Owned=FALSE;mClocks[I].Status=mRails[I].Status=EFI_SUCCESS;}
  memset(ClockRefs,0,sizeof(ClockRefs));memset(RailRefs,0,sizeof(RailRefs));memset(PathRefs,0,sizeof(PathRefs));
  Requests=Actions=Reads=Writes=FullReset=Acks=Irqs=Closes=AcquireNumber=OffNumber=RailNumber=SetPathNumber=0;
  AcquireClockFail=OffClockFail=RailFail=PathFail=ResetFail=CloseFail=ClearFail=AckFail=UnregisterFail=0;
  Delays=CauseAt=0;DropCause=PadOwned=PadActive=Queued=IrqInstalled=FailConnect=CommitConnect=InIrq=InjectClockCause=FALSE;Card=TRUE;HwHandler=NULL;Work=NULL;
  Tpl=TPL_APPLICATION;Boot=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=Locate,.CreateEvent=Create,.SignalEvent=Signal,.CloseEvent=Close};gBS=&Boot;
  FloorStatus=CardStatus=EFI_SUCCESS;FloorCalls=0;Rail.EnsureBootFloor=Floor;Rail.Revision=EFI_RPMH_CR_PROTOCOL_REVISION;
  CR_SDHC_IO Io={&Core,Ready,Read,Write,Stall,Barrier,sizeof(Regs),CR_SDHC_WINDOW_ALL};assert(CrSdhcInit(&Core,&Io)==CR_SUCCESS);
}
STATIC VOID Clean(VOID){EFI_STATUS Status=CrSdResourcesRelease(&Core);if(Status!=EFI_SUCCESS)fprintf(stderr,"cleanup status=%llx hand=%u power=%x pending=%x bus=%u/%u request=%u reset=%x\n",(unsigned long long)Status,PowerHandshake,R(0x29,1),Core.PowerPending,mBusValid,mBusOn,mRequestActive,R(0x2f,1));assert(Status==EFI_SUCCESS);assert(!ClockRefs[0]&&!ClockRefs[1]&&!RailRefs[0]&&!RailRefs[1]&&!PathRefs[0]&&!PathRefs[1]&&!PadOwned&&!HwHandler&&!mController);}
int main(void){
  /* Every late descriptor field must fail before protocol/provider effects. */
  CR_TARGET_SD_CONTEXT Target=*CrTargetGetSdContext();
  for(UINT32 I=0;I<14;++I){New();Target=*CrTargetGetSdContext();
    switch(I){case 0:Target.Revision=CR_TARGET_SD_REVISION+1;break;case 1:Target.Size--;break;
    case 2:Target.Variant=4;break;case 3:Target.Base++;break;case 4:Target.Bytes=0x400;break;
    case 5:Target.PowerInterrupt=31;break;case 6:Target.PowerTrigger=CR_INTERRUPT_TRIGGER_EDGE_RISING;break;
    case 7:Target.Clocks[1]=NULL;break;case 8:Target.Rails[1].VoltageMv=0;break;
    case 9:Target.Rails[1].Mode=0;break;case 10:Target.Paths[1].Provider=NULL;break;
    case 11:Target.Paths[1].Peak=Target.Paths[1].Average-1;break;
    case 12:Target.Gpio.CdPin=Target.Gpio.FirstPad;break;case 13:Target.CardDetectActiveLow=2;break;}
    mTestSdTarget=&Target;
    assert(CrSdResourcesAcquire(&Core)==EFI_COMPROMISED_DATA);
    assert(!mController&&!Requests&&!Actions&&!Writes&&!Reads&&!FloorCalls&&!mPowerWork);
  }
  New();mTestSdMissing=TRUE;assert(CrSdResourcesAcquire(&Core)==EFI_UNSUPPORTED&&!mController&&!Actions);
  New();Core.Io.MappingSize=0x254;assert(CrSdResourcesAcquire(&Core)==EFI_COMPROMISED_DATA&&!mController&&!Actions);
  New();Target=*CrTargetGetSdContext();Target.CardDetectActiveLow=TRUE;mTestSdTarget=&Target;
  assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);BOOLEAN Detected=TRUE;
  assert(CrSdResourcesCardPresent(&Detected)==EFI_SUCCESS&&!Detected);Card=FALSE;
  assert(CrSdResourcesCardPresent(&Detected)==EFI_SUCCESS&&Detected);
  Card=TRUE;CardStatus=EFI_WARN_UNKNOWN_GLYPH;
  assert(CrSdResourcesCardPresent(&Detected)==EFI_WARN_UNKNOWN_GLYPH&&Detected);
  CardStatus=EFI_SUCCESS;Clean();

  /* Same-CALLBACK waits must service IRQ effects inline. Neither a queued
   * callback nor a reset-register poll can complete the power transaction. */
  New();PowerHandshake=TRUE;assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);
  Tpl=TPL_CALLBACK;assert(CrSdhcPowerMask(&Core,TRUE)==CR_SUCCESS);
  assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_SUCCESS);
  UINT32 BeforeAck=Acks;V(0x3e,2,BIT3);
  assert(CrSdhcSdrSpeed(&Core,FALSE)==CR_SUCCESS&&Acks>BeforeAck&&!CauseAt);
  assert(CrSdhcReset(&Core,1)==CR_SUCCESS&&!R(0x2f,1)&&!RailRefs[0]&&!RailRefs[1]);
  assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_SUCCESS);
  /* A changed 1.8V selector cannot borrow the previously acknowledged 3V
   * state; no IO_HIGH cause is a timeout, not success from an empty queue. */
  V(0x3e,2,BIT3);DropIoCause=TRUE;
  assert(CrSdhcSdrSpeed(&Core,FALSE)==EFI_TIMEOUT&&!mRequestActive&&Core.NeedsReset);
  DropIoCause=FALSE;
  V(0x3e,2,BIT3);HostControlStatus=EFI_DEVICE_ERROR;
  assert(CrSdhcSdrSpeed(&Core,FALSE)==EFI_DEVICE_ERROR&&!mRequestActive);
  HostControlStatus=EFI_WARN_UNKNOWN_GLYPH;
  assert(CrSdhcSdrSpeed(&Core,FALSE)==EFI_WARN_UNKNOWN_GLYPH&&!mRequestActive);
  HostControlStatus=EFI_SUCCESS;
  assert(CrSdhcReset(&Core,1)==EFI_SUCCESS&&!Core.NeedsReset);
  assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_SUCCESS);
  Clean();PowerHandshake=FALSE;DeferredCause=0;
  New();Rail.EnsureBootFloor=NULL;assert(CrSdResourcesAcquire(&Core)==EFI_UNSUPPORTED&&!ClockRefs[0]&&!PathRefs[0]&&!PadOwned);Clean();
  New();FloorStatus=EFI_TIMEOUT;assert(CrSdResourcesAcquire(&Core)==EFI_TIMEOUT&&FloorCalls==1&&!ClockRefs[0]&&!PathRefs[0]&&!Writes);Clean();
  New();FloorStatus=EFI_WARN_UNKNOWN_GLYPH;assert(CrSdResourcesAcquire(&Core)==EFI_NOT_READY&&FloorCalls==1&&!ClockRefs[0]&&!PathRefs[0]&&!Writes);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);assert(CrInterruptIsRegistered(&mIrq)&&mIrqOwned);
  assert(CrSdhcReset(&Core,1)==CR_SUCCESS&&CrSdhcColdInit(&Core)==CR_SUCCESS);
  assert(CrSdhcPowerMask(&Core,TRUE)==CR_SUCCESS);Tpl=TPL_CALLBACK;
  assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_SUCCESS);assert(Irqs==1&&Acks==1&&mBusValid&&mBusOn&&PadActive&&RailRefs[0]&&RailRefs[1]);
  UINT32 Count=Requests;assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_SUCCESS&&Requests==Count);
  assert(CrSdResourcesSetClock(25000000)==EFI_SUCCESS&&ClockRefs[1]==1);
  assert(CrSdResourcesRequestPower(&Core,FALSE)==EFI_SUCCESS&&!PadActive&&!RailRefs[0]&&!RailRefs[1]);
  BOOLEAN P=FALSE;assert(CrSdResourcesCardPresent(&P)==EFI_SUCCESS&&P);Card=FALSE;assert(CrSdResourcesCardPresent(&P)==EFI_SUCCESS&&!P);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);assert(CrSdhcPowerMask(&Core,TRUE)==CR_SUCCESS);
  assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_SUCCESS&&Tpl==TPL_APPLICATION&&Irqs==1&&Acks==1);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);DropCause=TRUE;assert(CrSdResourcesRequestPower(&Core,TRUE)==EFI_TIMEOUT&&Delays==5000000&&Acks==0&&!mBusValid);Clean();
  for(UINT32 F=1;F<=2;++F){New();AcquireClockFail=F;assert(CrSdResourcesAcquire(&Core)==EFI_TIMEOUT);
    OffClockFail=1;assert(CrSdResourcesRelease(&Core)==EFI_TIMEOUT);assert(mController&&PathRefs[0]&&PathRefs[1]&&ClockRefs[0]&&!mHostStopped);
    OffClockFail=0;ResetFail=1;assert(CrSdResourcesRelease(&Core)==EFI_DEVICE_ERROR);assert(mController&&ClockRefs[0]&&ClockRefs[1]&&PathRefs[0]&&!mHostStopped);
    ResetFail=0;Clean();}
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);OffClockFail=1;
  assert(CrSdResourcesSetClock(50000000)==EFI_TIMEOUT&&!mAccess&&ClockRefs[1]);
  OffClockFail=OffNumber+1;assert(CrSdResourcesRelease(&Core)==EFI_TIMEOUT&&PadOwned&&ClockRefs[0]&&PathRefs[1]);OffClockFail=0;Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);InjectClockCause=TRUE;Tpl=TPL_CALLBACK;
  assert(CrSdResourcesSetClock(25000000)==EFI_SUCCESS&&HwHandler&&!mIrqPaused&&R(0x240,4)==2);
  Fire();assert(Irqs==1&&Core.PowerPending==2);assert(CrSdResourcesServicePower(&Core)==EFI_SUCCESS&&mBusValid&&mBusOn);Clean();
  New();PathFail=2;assert(CrSdResourcesAcquire(&Core)==EFI_TIMEOUT);Clean();
  New();FailConnect=CommitConnect=TRUE;UnregisterFail=1;assert(CrSdResourcesAcquire(&Core)==EFI_DEVICE_ERROR&&mIrqOwned&&HwHandler);
  assert(CrSdResourcesRelease(&Core)==EFI_DEVICE_ERROR&&PadOwned&&ClockRefs[1]&&mPowerWork);UnregisterFail=0;Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);CloseFail=1;assert(CrSdResourcesRelease(&Core)==EFI_DEVICE_ERROR&&mPowerWork&&PadOwned&&!HwHandler);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);V(0x240,4,CR_SDHC_POWER_ON);ClearFail=1;
  assert(CrSdResourcesServicePower(&Core)==EFI_DEVICE_ERROR&&Core.PowerPending==2&&Acks==0&&!RailRefs[0]);
  ClearFail=0;AckFail=1;assert(CrSdResourcesServicePower(&Core)==EFI_DEVICE_ERROR&&Core.PowerPending==2&&RailRefs[0]&&RailRefs[1]);
  assert(CrSdResourcesServicePower(&Core)==EFI_SUCCESS&&!Core.PowerPending&&mBusValid&&mBusOn);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);V(0x240,4,CR_SDHC_IO_LOW);
  assert(CrSdResourcesServicePower(&Core)==EFI_UNSUPPORTED&&R(0x24c,4)==8&&!Core.PowerPending&&!RailRefs[0]);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);V(0x240,4,CR_SDHC_POWER_ON);RailFail=2;
  assert(CrSdResourcesServicePower(&Core)==EFI_TIMEOUT&&R(0x24c,4)==10&&!mBusValid&&mRails[1].Owned);
  V(0x240,4,CR_SDHC_POWER_ON);assert(CrSdResourcesServicePower(&Core)==EFI_SUCCESS&&RailRefs[1]==1);Clean();
  New();assert(CrSdResourcesAcquire(&Core)==EFI_SUCCESS);Tpl=TPL_NOTIFY;
  assert(CrSdResourcesFreeze(&Core)==EFI_SUCCESS&&!HwHandler&&mFrozen&&mAccess&&CrSdResourcesReady()==EFI_SUCCESS);
  UINT32 Before=Actions,BeforeReads=Reads,BeforeWrites=Writes;BOOLEAN Q=FALSE;
  assert(CrSdResourcesSetClock(25000000)==EFI_ACCESS_DENIED&&CrSdResourcesCardPresent(&Q)==EFI_ACCESS_DENIED);
  assert(CrSdResourcesServicePower(&Core)==EFI_ACCESS_DENIED&&CrSdResourcesRequestPower(&Core,TRUE)==EFI_ACCESS_DENIED);
  assert(CrSdResourcesRelease(&Core)==EFI_ACCESS_DENIED&&Actions==Before&&Reads==BeforeReads&&Writes==BeforeWrites&&PadOwned&&ClockRefs[0]&&PathRefs[0]);
  Work((VOID *)1,NULL);assert(Actions==Before&&Writes==BeforeWrites);
  assert(CrSdhcClock(&Core,FALSE)==CR_SUCCESS&&CrSdhcReset(&Core,1)==CR_SUCCESS);
  puts("Actual native SD resources+OSKAL+PIO: generation ACK, callback-TPL IRQ progress, retained cleanup, post-EBS guards PASS");return 0;
}
