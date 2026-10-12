/** UCSI production monitor lifecycle regressions. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "../Driver/UCSICrDxe/UCSICrDxe.c"
#include <Library/UcsiTargetLib.h>
#define mTarget mUcsiFixtureTarget
#ifdef UCSI_TEST_SM8350
#include "../Target/Sm8350/CrUcsiTarget.c"
#else
#include "../Target/Sm8450/CrUcsiTarget.c"
#endif
#undef mTarget

EFI_GUID gEfiSmemCrProtocolGuid, gEfiGpioCrProtocolGuid;
EFI_GUID gEfiUsbTypeCCrProtocolGuid, gEfiEventExitBootServicesGuid;
EFI_GUID gEfiPilCrProtocolGuid;
EFI_GUID gEfiRpmhCrProtocolGuid, gEfiPmicGlinkCrProtocolGuid;
static UINTN SubscriberStates, SubscriberPackets;
static EFI_PMIC_GLINK_CR_STATE SubscriberState;
static int SubscriberIdentity;
static EFI_STATUS GlinkSendStatus;
static UINTN GlinkSends;
static EFI_RPMH_CR_PROTOCOL Rpmh;
static UINTN SupplyAcquires, SupplyReleases;
static BOOLEAN SupplyPowered;
static EFI_STATUS SupplyStatus, ReleaseStatus;
static EFI_PIL_CR_PROTOCOL PilLoader;
static BOOLEAN PilRunning, PilPublished;
static BOOLEAN ClientRegistered;
static CR_PIL_CLIENT_QUIESCE ClientCallback;
static VOID *ClientContext;
static EFI_STATUS RegisterStatus, UnregisterStatus;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS = &Services;
static EFI_SMEM_CR_PROTOCOL Smem;
static EFI_GPIO_CR_PROTOCOL Gpio;
static EFI_EVENT_NOTIFY TimerCallback, ExitCallback, WorkCallback;
static BOOLEAN TimerOpen, ExitOpen, WorkOpen, Installed, WorkQueued, KeepIrqOnFailure;
static EFI_STATUS IrqStartStatus, IrqStopStatus, SignalStatus;
static UINTN IrqStarts, IrqStops, Signals;
static UINTN Creates, FailCreateAt, Arms, Closes, Polls;
static UINT64 TimeUs, LastDelay;
static EFI_STATUS TimerStatus, InstallStatus, RemoveStatus, PollStatus, BindStatus;
static EFI_TPL CurrentTpl;
static UINT32 NextGeneration;
static BOOLEAN NextReady;
static UINT32 ExitInject, LocateCalls, CloseFailAt;
static BOOLEAN SupplyReturnsOwned, OrientationReturnsOwned;
static BOOLEAN InIrqStart;
static VOID InjectExit(UINT32 At) { if(ExitInject==At) { ExitInject=0; ExitCallback((EFI_EVENT)2,NULL); } }
static EFI_STATUS EFIAPI RegisterPilClient(EFI_PIL_CR_PROTOCOL *This, VOID *Owner,
                                           CR_PIL_CLIENT_QUIESCE Callback, VOID *Context)
{
  assert(This==&PilLoader && PilPublished && Owner==&mProtocol && Callback!=NULL);
  assert(!ClientRegistered && !SupplyPowered && !mIpcc.Registered && !Installed);
  if (EFI_ERROR(RegisterStatus)) return RegisterStatus;
  ClientRegistered=TRUE; ClientCallback=Callback; ClientContext=Context;
  InjectExit(1);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI UnregisterPilClient(EFI_PIL_CR_PROTOCOL *This, VOID *Owner)
{
  assert(This==&PilLoader && PilPublished && Owner==&mProtocol && ClientRegistered);
  assert(mStopped && !mIpcc.Registered && !mServicing && !SupplyPowered);
  if (EFI_ERROR(UnregisterStatus)) return UnregisterStatus;
  ClientRegistered=FALSE; ClientCallback=NULL; ClientContext=NULL;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PilState(EFI_PIL_CR_PROTOCOL *This, EFI_PIL_CR_STATE *State)
{ assert(This==&PilLoader && PilPublished); ZeroMem(State,sizeof(*State)); State->Ready=PilRunning; return PilRunning?EFI_SUCCESS:EFI_NOT_READY; }
static EFI_STATUS EFIAPI AcquireSupply(EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name,
                                      UINT32 Mv, UINT8 Mode, BOOLEAN *Owned)
{
  assert(This==&Rpmh && strcmp(Name,"ldob2")==0 && Mv==3072 && Mode==7);
  assert(ExitOpen && TimerOpen && WorkOpen && !Installed && Polls==0 && !SupplyPowered);
  ++SupplyAcquires; *Owned=SupplyPowered=SupplyReturnsOwned;
  InjectExit(4);
  return SupplyStatus; /* a failed write can still have powered the PMIC */
}
static EFI_STATUS EFIAPI ReleaseSupply(EFI_RPMH_CR_PROTOCOL *This, CONST CHAR8 *Name, BOOLEAN Enable)
{
  assert(This==&Rpmh && strcmp(Name,"ldob2")==0 && !Enable && SupplyPowered && mStopped);
  ++SupplyReleases;
  if(!EFI_ERROR(ReleaseStatus)) SupplyPowered=FALSE;
  return ReleaseStatus;
}

VOID *EFIAPI CopyMem(VOID *Dst, CONST VOID *Src, UINTN Size) { return memcpy(Dst,Src,Size); }
VOID *EFIAPI ZeroMem(VOID *Dst, UINTN Size) { return memset(Dst,0,Size); }
VOID *EFIAPI SetMem(VOID *Dst, UINTN Size, UINT8 Value) { return memset(Dst,Value,Size); }
INTN EFIAPI CompareMem(CONST VOID *A, CONST VOID *B, UINTN Size) { return memcmp(A,B,Size); }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level) { (VOID)Level; return FALSE; }
VOID EFIAPI DebugPrint(UINTN Level, CONST CHAR8 *Format, ...) { (VOID)Level; (VOID)Format; }
UINT64 EFIAPI GetPerformanceCounter(VOID) { return TimeUs; }
UINT64 EFIAPI GetTimeInNanoSecond(UINT64 Ticks) { return Ticks * 1000U; }
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *Value, UINT32 Compare, UINT32 Exchange)
{ __atomic_compare_exchange_n(Value,&Compare,Exchange,FALSE,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST); return Compare; }
#ifndef UCSI_REAL_IPCC
UINT32 EFIAPI MmioWrite32(UINTN Address, UINT32 Value)
{ assert(Address==CrUcsiGetTarget()->IpccBase+0xc && Value==(3U<<16)); return Value; }
CR_STATUS CrIpccStart(CR_IPCC *Ipcc, UINTN Base, CONST CR_INTERRUPT_CONFIG *Config,
                      CR_IPCC_WAKE WakeCallback, VOID *Context)
{
  InIrqStart=TRUE;
  assert(Ipcc==&mIpcc && Base==CrUcsiGetTarget()->IpccBase && WorkOpen && ExitOpen &&
    SupplyPowered==(CrUcsiGetTarget()->PdRail!=NULL) && !Installed);
  assert(Config->InterruptNumber==CrUcsiGetTarget()->IpccSource && Config->TriggerType==CR_INTERRUPT_TRIGGER_LEVEL_HIGH);
  assert(ClientRegistered && mPilOwner==&PilLoader);
  ++IrqStarts; Ipcc->Wake=WakeCallback; Ipcc->Context=Context;
  Ipcc->Registered=Ipcc->SignalOwned=!EFI_ERROR(IrqStartStatus) || KeepIrqOnFailure;
  Ipcc->Stopping=EFI_ERROR(IrqStartStatus);
  InjectExit(3);
  InIrqStart=FALSE;
  return IrqStartStatus;
}
CR_STATUS CrIpccStop(CR_IPCC *Ipcc)
{
  assert(Ipcc==&mIpcc && mStopped&&!InIrqStart); Ipcc->Stopping=1;
  if(Ipcc->Registered) { ++IrqStops; if(EFI_ERROR(IrqStopStatus)) return IrqStopStatus; }
  Ipcc->Registered=Ipcc->SignalOwned=FALSE; Ipcc->Pending=0;
  return EFI_SUCCESS;
}
CR_STATUS CrIpccService(CR_IPCC *Ipcc, BOOLEAN *Received)
{
  assert(Ipcc==&mIpcc && !mStopped); *Received=Ipcc->Pending!=0; Ipcc->Pending=0;
  return Ipcc->Error;
}
CR_STATUS CrIpccStartGuarded(CR_IPCC *I,UINTN B,CONST CR_INTERRUPT_CONFIG *C,
 CR_IPCC_WAKE W,VOID *X,CR_IPCC_ADMIT A,VOID *AC){
 assert(A!=NULL&&A(AC));EFI_STATUS S=CrIpccStart(I,B,C,W,X);
 if(!A(AC)){EFI_STATUS R=CrIpccStop(I);return EFI_ERROR(R)?R:EFI_NOT_READY;}
 return S;
}
VOID CrIpccSend(UINTN Base) { MmioWrite32(Base+0xc,3U<<16); }
#else
static BOOLEAN IrqRegistered;
static UINT32 IpccControl,IpccEnables,PostExitEnables;
static BOOLEAN FireOnEnable;
static UINT32 ReceiveId;
UINT32 EFIAPI MmioRead32(UINTN A){assert(A>=CrUcsiGetTarget()->IpccBase&&A<CrUcsiGetTarget()->IpccBase+0x20);if(A-CrUcsiGetTarget()->IpccBase==8)return IpccControl;if(A-CrUcsiGetTarget()->IpccBase==0x10){UINT32 I=ReceiveId;ReceiveId=0xffffffffU;return I;}return 0xffffffffU;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){
  UINTN O=A-CrUcsiGetTarget()->IpccBase;assert(O==8||O==0xc||O==0x14||O==0x18||O==0x1c);
  if(O==8)IpccControl=V;
  if(O==0x14){++IpccEnables;if(mExiting)++PostExitEnables;}
  if(O==0x14&&FireOnEnable){FireOnEnable=FALSE;ReceiveId=3U<<16;mIpcc.Interrupt.Handler(mIpcc.Interrupt.Param);}
  return V;
}
CR_STATUS CrRegisterInterrupt(CR_INTERRUPT_CONFIG *C){
 assert(C==&mIpcc.Interrupt&&C->InterruptNumber==261);InIrqStart=TRUE;++IrqStarts;IrqRegistered=TRUE;
 InjectExit(3);InIrqStart=FALSE;return EFI_SUCCESS;
}
BOOLEAN CrInterruptIsRegistered(CR_INTERRUPT_CONFIG *C){assert(C==&mIpcc.Interrupt);return IrqRegistered;}
CR_STATUS CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *C){assert(C==&mIpcc.Interrupt&&!InIrqStart);++IrqStops;IrqRegistered=FALSE;return EFI_SUCCESS;}
VOID EFIAPI MemoryFence(VOID){}
#endif
static EFI_STATUS EFIAPI Signal(EFI_EVENT Event)
{
  assert(Event==(EFI_EVENT)3 && WorkOpen); ++Signals;
  if(!EFI_ERROR(SignalStatus)) WorkQueued=TRUE;
  return SignalStatus;
}
static EFI_STATUS EFIAPI SmemGetMock(EFI_SMEM_CR_PROTOCOL *This, UINT32 Host,
                                    UINT32 Item, UINT32 *Size, VOID **Address)
{ (void)This; (void)Host; (void)Item; (void)Size; (void)Address; return EFI_NOT_FOUND; }
static EFI_STATUS EFIAPI SmemAllocateMock(EFI_SMEM_CR_PROTOCOL *This, UINT32 Host,
                                         UINT32 Item, UINT32 Size, VOID **Address)
{ (void)This; (void)Host; (void)Item; (void)Size; (void)Address; return EFI_NOT_FOUND; }

VOID CrGlinkReset(CR_GLINK *Link)
{
  CR_GLINK_STATE State=Link->QrtrState; CR_GLINK_QRTR_RECEIVE Receive=Link->QrtrReceive;
  CR_GLINK_STATE LogState=Link->LogsState; CR_GLINK_QRTR_RECEIVE LogReceive=Link->LogsReceive;
  VOID *Context=Link->Context;
  if(State!=NULL) State(Context,FALSE);
  if(LogState!=NULL) LogState(Context,FALSE);
  ZeroMem(Link,sizeof(*Link)); Link->QrtrState=State; Link->QrtrReceive=Receive; Link->Context=Context;
  Link->LogsState=LogState; Link->LogsReceive=LogReceive;
}
CR_STATUS CrGlinkBind(CR_GLINK *Link, CONST CR_GLINK_SMEM *Protocol)
{ assert(Link==&mLink && Protocol->Context==&Smem && Protocol->Get!=NULL && Protocol->Allocate!=NULL); Link->Bound=!EFI_ERROR(BindStatus); return BindStatus; }
CR_STATUS CrGlinkServiceTx(CR_GLINK *Link) { assert(Link==&mLink && SupplyPowered==(CrUcsiGetTarget()->PdRail!=NULL)); return EFI_SUCCESS; }
EFI_STATUS CrGlinkPoll(CR_GLINK *Link)
{
  assert(Link==&mLink && Installed && TimerOpen && SupplyPowered==(CrUcsiGetTarget()->PdRail!=NULL));
  ++Polls;
  Link->Ready=NextReady;
  Link->Generation=NextGeneration;
  return PollStatus;
}
EFI_STATUS CrGlinkSend(CR_GLINK *Link, CONST VOID *Data, UINTN Length)
{ assert(Link==&mLink && Data!=NULL && Length!=0); ++GlinkSends; return GlinkSendStatus; }
EFI_STATUS CrGlinkSendQrtr(CR_GLINK *Link, CONST VOID *Data, UINTN Length)
{ assert(Link==&mLink && Data!=NULL && Length!=0); return EFI_NOT_READY; }
EFI_STATUS CrGlinkSendLogs(CR_GLINK *Link, CONST VOID *Data, UINTN Length)
{ assert(Link==&mLink && Data!=NULL && Length==16); return EFI_NOT_READY; }

static EFI_TPL EFIAPI Raise(EFI_TPL Tpl)
{ EFI_TPL Old=CurrentTpl; assert(Tpl>=Old); CurrentTpl=Tpl; return Old; }
static VOID EFIAPI Restore(EFI_TPL Tpl) { CurrentTpl=Tpl; }
static EFI_STATUS EFIAPI Locate(EFI_GUID *Guid, VOID *Registration, VOID **Protocol)
{
  assert(Registration==NULL);
  ++LocateCalls;
  if(Guid==&gEfiSmemCrProtocolGuid) *Protocol=&Smem;
  else if(Guid==&gEfiGpioCrProtocolGuid) *Protocol=&Gpio;
  else if(Guid==&gEfiPilCrProtocolGuid) { if(!PilPublished) return EFI_NOT_FOUND; *Protocol=&PilLoader; }
  else if(Guid==&gEfiRpmhCrProtocolGuid) *Protocol=&Rpmh;
  else return EFI_NOT_FOUND;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Pins(EFI_GPIO_CR_PROTOCOL *This, GpioConfigParams *Config)
{
  assert(This==&Gpio && Config->PinNumber==CrUcsiGetTarget()->OrientationGpio && !Config->OutputEnable);
  assert(Config->FunctionSel==GPIO_FUNC_NORMAL);
  assert(Config->Pull==(CrUcsiGetTarget()->LeaseOrientation?GPIO_PULL_UP:GPIO_PULL_NONE));
  assert(Config->DriveStrength==(CrUcsiGetTarget()->LeaseOrientation?GPIO_DRIVE_STRENGTH_2MA:GPIO_DRIVE_STRENGTH_UNCHANGE) && Config->OutputValue==GPIO_VALUE_UNCHANGE);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ReadPin(EFI_GPIO_CR_PROTOCOL *This, UINT16 Pin, UINT8 *Value)
{ assert(This==&Gpio && Pin==CrUcsiGetTarget()->OrientationGpio); *Value=1; return EFI_SUCCESS; }
static BOOLEAN OrientationOwned;
static EFI_STATUS OrientationAcquireStatus,OrientationReleaseStatus;
#ifdef UCSI_TEST_SM8350
#include <Library/GpioPinLeaseLib.h>
static CR_GPIO_PIN_LEASE_BANK LeaseBank;
static GpioDeviceContext LeaseContext;
static GpioConfigParams Pad;
VOID GpioReadConfig(GpioDeviceContext *C,GpioConfigParams *D){assert(C==&LeaseContext&&D->PinNumber==81);*D=Pad;}
GpioValueType GpioReadOutputVal(GpioDeviceContext *C,UINT16 N){assert(C==&LeaseContext&&N==81);return Pad.OutputValue;}
GpioValueType GpioReadInputVal(GpioDeviceContext *C,UINT16 N){assert(C==&LeaseContext&&N==81);return GPIO_VALUE_HIGH;}
CR_STATUS GpioCheckInterruptEnabled(GpioDeviceContext *C,UINT16 N,BOOLEAN *V){assert(C==&LeaseContext&&N==81);*V=FALSE;return CR_SUCCESS;}
CR_STATUS GpioApplyConfigChecked(GpioDeviceContext *C,CONST GpioConfigParams *D){assert(C==&LeaseContext&&D->PinNumber==81);Pad=*D;return EFI_SUCCESS;}
#endif
static EFI_STATUS EFIAPI AcquireOrientation(EFI_GPIO_CR_PROTOCOL *This,VOID *Owner,CONST GpioConfigParams *C,BOOLEAN *Owned) {
  assert(Owner==&mProtocol&&!OrientationOwned&&CrUcsiGetTarget()->LeaseOrientation);
  GpioConfigParams Copy=*C;Pins(This,&Copy);*Owned=FALSE;
  if(!OrientationReturnsOwned){InjectExit(2);return OrientationAcquireStatus;}
#ifdef UCSI_TEST_SM8350
  EFI_STATUS S=CrGpioPinLeaseAcquire(&LeaseBank,Owner,C,Owned);
  OrientationOwned=*Owned;
  if(S!=EFI_SUCCESS)return S;
#else
  *Owned=OrientationOwned=TRUE;
#endif
  InjectExit(2);return OrientationAcquireStatus;
}
static EFI_STATUS EFIAPI ReadOrientation(EFI_GPIO_CR_PROTOCOL *This,VOID *Owner,UINT16 Pin,UINT8 *Value) {
  assert(Owner==&mProtocol&&OrientationOwned);return ReadPin(This,Pin,Value);
}
static EFI_STATUS EFIAPI ReleaseOrientationMock(EFI_GPIO_CR_PROTOCOL *This,VOID *Owner,UINT16 Pin) {
  assert(This==&Gpio&&Owner==&mProtocol&&OrientationOwned&&Pin==81);
  if(OrientationReleaseStatus==EFI_SUCCESS){
#ifdef UCSI_TEST_SM8350
    assert(CrGpioPinLeaseRelease(&LeaseBank,Owner,Pin)==EFI_SUCCESS);
#endif
    OrientationOwned=FALSE;
  }return OrientationReleaseStatus;
}
static EFI_STATUS EFIAPI CreateTimer(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify,
                                   VOID *Context, EFI_EVENT *Event)
{
  assert((Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL) || Type==EVT_NOTIFY_SIGNAL) && Tpl==TPL_CALLBACK && Context==NULL);
  if(++Creates==FailCreateAt) return EFI_OUT_OF_RESOURCES;
  if(Type & EVT_TIMER) { TimerOpen=TRUE; TimerCallback=Notify; *Event=(EFI_EVENT)1; }
  else { WorkOpen=TRUE; WorkCallback=Notify; *Event=(EFI_EVENT)3; }
  Notify(*Event,Context); /* stale timer dispatch must do no work during setup */
  assert(Polls==0);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateExit(UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify,
                                  CONST VOID *Context, CONST EFI_GUID *Guid, EFI_EVENT *Event)
{
  assert(Type==EVT_NOTIFY_SIGNAL && Tpl==TPL_NOTIFY && Context==NULL);
  assert(Guid==&gEfiEventExitBootServicesGuid);
  if(++Creates==FailCreateAt) return EFI_OUT_OF_RESOURCES;
  ExitOpen=TRUE; ExitCallback=Notify; *Event=(EFI_EVENT)2;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Timer(EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Time)
{
  assert(Event==(EFI_EVENT)1 && TimerOpen);
  if(Type==TimerCancel) { assert(Time==0); return EFI_SUCCESS; }
  assert(Type==TimerRelative && (Time==200000 || Time==10000000) && Installed && ExitOpen && WorkOpen);
  LastDelay=Time;
  ++Arms;
  return TimerStatus;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT Event)
{
  if(Closes+1==CloseFailAt) {++Closes;return EFI_DEVICE_ERROR;}
  if(Event==(EFI_EVENT)1) { assert(TimerOpen); TimerOpen=FALSE; }
  else if(Event==(EFI_EVENT)2) { assert(ExitOpen); ExitOpen=FALSE; }
  else { assert(Event==(EFI_EVENT)3 && WorkOpen && !mIpcc.Registered); WorkOpen=FALSE; WorkQueued=FALSE; }
  ++Closes;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Publish(EFI_HANDLE *Handle, ...)
{
  va_list Args;
  assert(*Handle==(EFI_HANDLE)0x1234 && TimerOpen && ExitOpen && WorkOpen && mIpcc.Registered && Arms==0 && SupplyPowered==(CrUcsiGetTarget()->PdRail!=NULL));
  va_start(Args,Handle);
  assert(va_arg(Args,EFI_GUID *)==&gEfiUsbTypeCCrProtocolGuid);
  assert(va_arg(Args,VOID *)==&mProtocol);
  assert(va_arg(Args,EFI_GUID *)==&gEfiPmicGlinkCrProtocolGuid);
  assert(va_arg(Args,VOID *)==&mPmicProtocol && va_arg(Args,VOID *)==NULL);
  va_end(Args);
  Installed=!EFI_ERROR(InstallStatus);
  InjectExit(5);
  return InstallStatus;
}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE Handle, ...)
{
  va_list Args;
  assert(Handle==(EFI_HANDLE)0x1234 && Installed && mStopped);
  va_start(Args,Handle);
  assert(va_arg(Args,EFI_GUID *)==&gEfiUsbTypeCCrProtocolGuid);
  assert(va_arg(Args,VOID *)==&mProtocol);
  assert(va_arg(Args,EFI_GUID *)==&gEfiPmicGlinkCrProtocolGuid);
  assert(va_arg(Args,VOID *)==&mPmicProtocol && va_arg(Args,VOID *)==NULL);
  va_end(Args);
  if(!EFI_ERROR(RemoveStatus)) Installed=FALSE;
  return RemoveStatus;
}
static void Fixture(void)
{
  ZeroMem(&mLink,sizeof(mLink)); ZeroMem(&mPmic,sizeof(mPmic)); ZeroMem(&mProtocol,sizeof(mProtocol));
  ZeroMem(&mIpcc,sizeof(mIpcc));
  ZeroMem(mSubscribers,sizeof(mSubscribers)); ZeroMem(&mPmicTransport,sizeof(mPmicTransport));
  GlinkSendStatus=EFI_NOT_READY; GlinkSends=SubscriberStates=SubscriberPackets=0;
  mTimer=mExit=mWork=NULL; mPublished=mStopped=mPeerReady=mServicing=mIrqLogged=mRxPending=FALSE;
  mExiting=mEntryBusy=mOwnershipUncertain=FALSE;
  IrqStartStatus=IrqStopStatus=SignalStatus=EFI_SUCCESS;
  IrqStarts=IrqStops=Signals=0; KeepIrqOnFailure=WorkQueued=FALSE;
  ExitInject=LocateCalls=CloseFailAt=0;SupplyReturnsOwned=OrientationReturnsOwned=TRUE;
  InIrqStart=FALSE;
#ifdef UCSI_REAL_IPCC
  IrqRegistered=FireOnEnable=FALSE;ReceiveId=0xffffffffU;IpccControl=1;IpccEnables=PostExitEnables=0;
#endif
  mPdSupplyOwned=SupplyPowered=FALSE;
  mPilOwner=NULL; ClientRegistered=FALSE; ClientCallback=NULL; ClientContext=NULL;
  RegisterStatus=UnregisterStatus=EFI_SUCCESS;
  SupplyAcquires=SupplyReleases=0;
  SupplyStatus=ReleaseStatus=EFI_SUCCESS;
  Rpmh.Revision=EFI_RPMH_CR_PROTOCOL_REVISION;
  Rpmh.AcquireVreg=AcquireSupply; Rpmh.RpmhEnableVreg=ReleaseSupply;
  TimerOpen=ExitOpen=WorkOpen=Installed=FALSE;
  Creates=FailCreateAt=Arms=Closes=Polls=0;
  TimerStatus=InstallStatus=RemoveStatus=PollStatus=BindStatus=EFI_SUCCESS;
  TimeUs=NextGeneration=LastDelay=0; NextReady=FALSE; CurrentTpl=TPL_APPLICATION;
  Services.LocateProtocol=Locate; Services.CreateEvent=CreateTimer;
  Services.CreateEventEx=CreateExit; Services.SetTimer=Timer; Services.CloseEvent=Close;
  Services.InstallMultipleProtocolInterfaces=Publish; Services.UninstallMultipleProtocolInterfaces=Remove;
  Services.RaiseTPL=Raise; Services.RestoreTPL=Restore;
  Services.SignalEvent=Signal;
  Smem.Revision=EFI_SMEM_CR_PROTOCOL_REVISION;
  Smem.GetItem=SmemGetMock; Smem.AllocateItem=SmemAllocateMock;
  PilPublished=PilRunning=TRUE; PilLoader.Revision=EFI_PIL_CR_PROTOCOL_REVISION; PilLoader.GetState=PilState;
  PilLoader.RegisterClient=RegisterPilClient; PilLoader.UnregisterClient=UnregisterPilClient;
  Gpio.ConfigGpio=Pins; Gpio.ReadIoValue=ReadPin;
  OrientationOwned=FALSE;OrientationAcquireStatus=OrientationReleaseStatus=EFI_SUCCESS;
  Gpio.Revision=0x10002;Gpio.AcquirePin=AcquireOrientation;Gpio.ReadPinValue=ReadOrientation;Gpio.ReleasePin=ReleaseOrientationMock;
#ifdef UCSI_TEST_SM8350
  ZeroMem(&LeaseBank,sizeof(LeaseBank));ZeroMem(&LeaseContext,sizeof(LeaseContext));
  LeaseContext.TlmmBaseAddress=0xf000000;LeaseContext.PinCount=180;LeaseContext.EgpioFunction=GPIO_FUNC_15;
  LeaseContext.RegCfg[0]=(GpioConfigRegInfo){.MuxSelMsk=0x3c,.PullMsk=3,.DriveStrengthMsk=0x1c0,.OutputEnableMsk=0x200,.InputMsk=1,.OutputMsk=2,.InterruptEnMsk=1};
  LeaseContext.GpioPins[81]=(GpioPinInfo){.PinAddress=0xf000000+81*0x1000,.pRegCfg=&LeaseContext.RegCfg[0],.pTileInfo=&LeaseContext.Tiles[0],.FuncMuxMap=3};
  Pad=(GpioConfigParams){81,FALSE,GPIO_FUNC_1,GPIO_PULL_DOWN,GPIO_DRIVE_STRENGTH_8MA,GPIO_VALUE_HIGH};
  assert(CrGpioPinLeaseInit(&LeaseBank,&LeaseContext)==EFI_SUCCESS);
#endif
}
/* Existing transport-change scenarios explicitly supply an incoming IRQ. */
static void Tick(void) { mIpcc.Pending=1; WorkQueued=FALSE; WorkCallback((EFI_EVENT)3,NULL); }
static void Watchdog(void) { TimerCallback((EFI_EVENT)1,NULL); }
static VOID EFIAPI SubscriberChange(VOID *Context, CONST EFI_PMIC_GLINK_CR_STATE *State)
{
  assert(Context==&SubscriberIdentity && CurrentTpl==TPL_NOTIFY);
  ++SubscriberStates; SubscriberState=*State;
}
static VOID EFIAPI SubscriberReceive(VOID *Context, CONST UINT8 *Data, UINTN Length)
{
  assert(Context==&SubscriberIdentity && CurrentTpl==TPL_NOTIFY && Length==24 && PacketOwner(Data)==32778);
  ++SubscriberPackets;
  /* Reentry is explicitly denied even if a client violates the pure callback contract. */
  assert(PmicSend(&mPmicProtocol,32778,&SubscriberIdentity,Data,Length)==EFI_NOT_READY);
  assert(PmicUnregister(&mPmicProtocol,32778,&SubscriberIdentity)==EFI_NOT_READY);
}
int main(void)
{
  EFI_USB_TYPEC_STATE State;
  /* Idle timer observes PIL liveness, with no RX FIFO/status polling. An
     IPCC wake handles a fresh incoming message without waiting for it. */
  Fixture(); NextReady=TRUE;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  Watchdog(); assert(Polls==1 && mPeerReady);
  mPmic.Stage=6; mPmic.Operation=0; mPmic.PanEnabled=TRUE;
  mPmic.ReadPending=mPmic.WritePending=FALSE;
  for(UINTN I=0;I<1000;++I) { TimeUs+=1000000; Watchdog(); }
  assert(Polls==1 && LastDelay==10000000 && !mStopped);
  mIpcc.Pending=1; assert(mIpcc.Wake(mIpcc.Context)==EFI_SUCCESS && WorkQueued);
  WorkQueued=FALSE; WorkCallback((EFI_EVENT)3,NULL);
  assert(Polls==2 && mIrqLogged);
  mPmic.State.PdReady=mPmic.State.Connected=TRUE;
  SignalStatus=EFI_DEVICE_ERROR;
  assert(SetDataRole(&mProtocol,UsbTypeCDevice)==EFI_DEVICE_ERROR && mPmic.RequestedCommand==0);
  SignalStatus=EFI_SUCCESS;
  assert(SetDataRole(&mProtocol,UsbTypeCDevice)==EFI_SUCCESS && WorkQueued);
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS && !WorkOpen && !mIpcc.Registered);

  Fixture(); IrqStartStatus=EFI_DEVICE_ERROR;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_DEVICE_ERROR);
  assert(!Installed && !TimerOpen && !ExitOpen && !WorkOpen && !SupplyPowered);
  Fixture(); IrqStartStatus=IrqStopStatus=EFI_DEVICE_ERROR; KeepIrqOnFailure=TRUE;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  assert(mStopped && mIpcc.Registered && WorkOpen && ExitOpen && SupplyPowered && !Installed && ClientRegistered);
  WorkCallback((EFI_EVENT)3,NULL); assert(Polls==0);
  IrqStopStatus=EFI_SUCCESS;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS && !WorkOpen && !SupplyPowered);
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  IrqStopStatus=EFI_DEVICE_ERROR;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR && Installed && mIpcc.Registered);
  assert(TimerOpen && ExitOpen && WorkOpen && SupplyPowered && SupplyReleases==0);
  ExitCallback((EFI_EVENT)2,NULL); WorkCallback((EFI_EVENT)3,NULL);
  assert(Polls==0 && SupplyReleases==0);
  IrqStopStatus=EFI_SUCCESS;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED && WorkOpen && Installed);

  for(UINTN Fail=1;Fail<=3;++Fail) {
    Fixture(); FailCreateAt=Fail;
    assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_OUT_OF_RESOURCES);
    assert(!Installed && !TimerOpen && !ExitOpen && !WorkOpen && mStopped && Polls==0);
    assert(SupplyAcquires==0 && SupplyReleases==0);
  }
  Fixture(); BindStatus=EFI_COMPROMISED_DATA;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_COMPROMISED_DATA && Creates==0);
  Fixture(); InstallStatus=EFI_ACCESS_DENIED;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_ACCESS_DENIED && Closes==3);
  assert(SupplyAcquires==1 && SupplyReleases==1 && !SupplyPowered && !mPdSupplyOwned);
  Fixture(); SupplyStatus=EFI_DEVICE_ERROR;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_DEVICE_ERROR);
  assert(SupplyAcquires==1 && SupplyReleases==1 && !SupplyPowered && !TimerOpen && !ExitOpen);
  Fixture(); SupplyStatus=ReleaseStatus=EFI_DEVICE_ERROR;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  assert(!Installed && TimerOpen && ExitOpen && mStopped && mPdSupplyOwned && SupplyPowered);
  Tick(); assert(Polls==0);
  ReleaseStatus=EFI_SUCCESS;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
  assert(!TimerOpen && !ExitOpen && !SupplyPowered && !mPdSupplyOwned);
  Fixture(); TimerStatus=EFI_DEVICE_ERROR;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_DEVICE_ERROR);
  assert(!Installed && !mPublished && !TimerOpen && !ExitOpen && mStopped);
  assert(SupplyReleases==1 && !SupplyPowered);
  Fixture(); TimerStatus=EFI_DEVICE_ERROR; RemoveStatus=EFI_ACCESS_DENIED;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  assert(Installed && mPublished && TimerOpen && ExitOpen && mStopped);
  assert(SupplyPowered && SupplyReleases==0);
  assert(GetState(&mProtocol,&State)==EFI_NOT_READY && State.LastError==EFI_DEVICE_ERROR);
  Fixture();
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS && Arms==1 && Polls==0);
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_ALREADY_STARTED);
  Tick(); assert(Polls==1 && Arms==2 && !mStopped);
  TimeUs=6000000; Tick(); /* Shell remains monitored after waiting for firmware. */
  assert(Polls==2 && Arms==3);
  NextReady=TRUE; Tick(); assert(mPeerReady && !mWaitLogged);
  mPmic.State.PdReady=mPmic.State.Connected=TRUE;
  mPmic.State.DataRole=UsbTypeCHost; mPmic.State.LastError=EFI_SUCCESS;
  Tick();
  assert(GetState(&mProtocol,&State)==EFI_SUCCESS && State.Orientation==UsbTypeCReverse);
  assert(SetDataRole(&mProtocol,UsbTypeCDevice)==EFI_SUCCESS);
  assert(mPmic.State.DataRole==UsbTypeCHost && CurrentTpl==TPL_APPLICATION);
  NextGeneration=1; Tick(); /* close/reopen in one poll must reset the old engine */
  assert(mPeerReady && !mPmic.State.Connected && mPmic.RequestedCommand==0);
  /* A notifier appearing after a fault fences UCSI until charger_pd is UP.
     A DOWN/UP pair between monitor ticks must still discard the old state. */
  mPmic.Faulted=TRUE; mPmic.State.LastError=EFI_TIMEOUT;
  mQrtr.PdKnown=TRUE; mQrtr.PdReady=FALSE; ++mQrtr.PdGeneration;
  Tick(); assert(!mPeerReady && !mPmic.Faulted && !mPmic.State.Connected);
  mQrtr.PdReady=TRUE; ++mQrtr.PdGeneration; Tick();
  assert(mPeerReady && !mPmic.Faulted);
  mPmic.State.Connected=mPmic.State.DpActive=mPmic.State.PdReady=TRUE;
  mQrtr.PdGeneration+=2; Tick();
  assert(mPeerReady && !mPmic.State.Connected && !mPmic.State.DpActive);
  CrQrtrOnline(&mQrtr,TRUE); Tick(); assert(!mPeerReady && mQrtr.PdKnown);
  mQrtr.PdReady=TRUE; ++mQrtr.PdGeneration; Tick(); assert(mPeerReady);
  CrQrtrOnline(&mQrtr,FALSE); Tick(); assert(!mPeerReady && mQrtr.PdKnown);
  CrQrtrOnline(&mQrtr,TRUE); Tick(); assert(!mPeerReady); /* no RPMSG fallback after known DOWN */
  mQrtr.PdReady=TRUE; ++mQrtr.PdGeneration; Tick(); assert(mPeerReady);
  mPmic.State.PdReady=mPmic.State.Connected=mPmic.State.DpActive=TRUE;
  mPmic.State.PowerSource=mPmic.State.Hpd=TRUE;
  TimerStatus=EFI_DEVICE_ERROR; Tick();
  assert(mStopped && GetState(&mProtocol,&State)==EFI_NOT_READY);
  assert(!State.Connected && !State.DpActive && !State.Hpd && !State.PowerSource);
  assert(State.LastError==EFI_DEVICE_ERROR && SetDataRole(&mProtocol,UsbTypeCHost)==EFI_NOT_READY);
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  PollStatus=EFI_COMPROMISED_DATA; Tick();
  assert(mStopped && mPmic.Faulted && Arms==1);
  Fixture(); PilRunning=FALSE;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  Tick(); assert(Polls==0 && Arms==2 && !mStopped);
  assert(GetState(&mProtocol,&State)==EFI_NOT_READY);
  assert(SetDataRole(&mProtocol,UsbTypeCHost)==EFI_NOT_READY);
  PilRunning=TRUE; NextReady=TRUE; Tick(); assert(Polls==1 && mPeerReady);
  mPmic.State.PdReady=mPmic.State.Connected=mPmic.State.DpActive=TRUE;
  mPmic.State.PowerSource=mPmic.State.Hpd=TRUE;
  PilRunning=FALSE;
  assert(GetState(&mProtocol,&State)==EFI_NOT_READY && !State.Connected && !State.PowerSource && !State.Hpd);
  Tick(); assert(Polls==1 && !mPeerReady && !mStopped);
  PilRunning=TRUE; Tick(); assert(Polls==2 && mPeerReady && !mRebind);
  mPmic.State.PdReady=mPmic.State.Connected=TRUE;
  PilPublished=FALSE; /* unload must not call the vanished protocol's code */
  assert(GetState(&mProtocol,&State)==EFI_NOT_READY && !State.Connected);
  Tick(); assert(Polls==2 && !mPeerReady && !mStopped && mPilLoader==NULL);
  PilPublished=TRUE; Tick(); assert(Polls==3 && mPeerReady && !mRebind);
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  ExitCallback((EFI_EVENT)2,NULL);
  Tick(); assert(mStopped && Polls==0 && Arms==1);
  assert(SupplyReleases==1 && !SupplyPowered && !mPdSupplyOwned);
  assert(GetState(&mProtocol,&State)==EFI_NOT_READY && SetPowerRole(&mProtocol,TRUE)==EFI_NOT_READY);
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  ReleaseStatus=EFI_DEVICE_ERROR;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_DEVICE_ERROR);
  assert(!Installed && TimerOpen && ExitOpen && SupplyPowered && mPdSupplyOwned && mStopped);
  ReleaseStatus=EFI_SUCCESS;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
  assert(!TimerOpen && !ExitOpen && !SupplyPowered && !mPdSupplyOwned);
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  ReleaseStatus=EFI_DEVICE_ERROR; ExitCallback((EFI_EVENT)2,NULL);
  assert(mPdSupplyOwned && SupplyPowered && mStopped);
  ReleaseStatus=EFI_SUCCESS; ExitCallback((EFI_EVENT)2,NULL);
  assert(!mPdSupplyOwned && !SupplyPowered && SupplyReleases==2);
  /* PIL invokes the actual synchronous barrier before this DXE's EBS event.
     A retained IRQ, interrupted service, or failed supply release blocks PAS. */
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  IrqStopStatus=EFI_DEVICE_ERROR;
  assert(ClientCallback(ClientContext)==EFI_DEVICE_ERROR && mIpcc.Registered && ClientRegistered);
  IrqStopStatus=EFI_SUCCESS; mServicing=TRUE;
  assert(ClientCallback(ClientContext)==EFI_NOT_READY && mIpcc.Registered && SupplyPowered);
  mServicing=FALSE; ReleaseStatus=EFI_TIMEOUT;
  assert(ClientCallback(ClientContext)==EFI_TIMEOUT && SupplyPowered);
  ReleaseStatus=EFI_SUCCESS;
  assert(ClientCallback(ClientContext)==EFI_SUCCESS && !SupplyPowered && mStopped);
  Tick(); assert(Polls==0 && Signals==0 && ClientRegistered);
  UnregisterStatus=EFI_ACCESS_DENIED;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED && ClientRegistered && WorkOpen);
  UnregisterStatus=EFI_SUCCESS;
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS && !ClientRegistered && mPilOwner==NULL);
  /* Reverse EBS event order still leaves a successful idempotent barrier. */
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  ExitCallback((EFI_EVENT)2,NULL);
  assert(ClientCallback(ClientContext)==EFI_SUCCESS && SupplyReleases==1);
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED && ClientRegistered);
  Fixture(); RegisterStatus=EFI_ACCESS_DENIED;
  assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_ACCESS_DENIED);
  assert(IrqStarts==0 && SupplyAcquires==0 && !ClientRegistered && !WorkOpen);
  /* Sole native transport: owner-qualified battery subscriptions, synchronous
     offline invalidation before IRQ/PIL stop, residency and foreign identity. */
  Fixture(); assert(UCSICrEntryPoint((EFI_HANDLE)0x1234,NULL)==EFI_SUCCESS);
  assert(PmicRegister(&mPmicProtocol,32778,&SubscriberIdentity,SubscriberReceive,SubscriberChange,&SubscriberIdentity)==EFI_SUCCESS);
  assert(SubscriberStates==1&&!SubscriberState.Online);
  assert(PmicRegister(&mPmicProtocol,32779,&SubscriberIdentity,SubscriberReceive,SubscriberChange,&SubscriberIdentity)==EFI_UNSUPPORTED);
  assert(PmicRegister(&mPmicProtocol,32778,&mProtocol,SubscriberReceive,SubscriberChange,&SubscriberIdentity)==EFI_ALREADY_STARTED);
  assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_ACCESS_DENIED&&!mStopped&&mIpcc.Registered);
  NextReady=TRUE;Tick();assert(SubscriberState.Online&&SubscriberState.AdspGeneration==1);
  UINT32 Generation=SubscriberState.PeerGeneration;
  GlinkSends=0;
  UINT8 Packet[24]={0x0a,0x80,0,0,1,0,0,0,0x30,0,0,0,2,0,0,0,1,0,0,0,0,0,0,0};
  assert(PmicSend(&mPmicProtocol,32778,&mProtocol,Packet,sizeof(Packet))==EFI_ACCESS_DENIED&&GlinkSends==0);
  assert(PmicSend(&mPmicProtocol,32779,&SubscriberIdentity,Packet,sizeof(Packet))==EFI_INVALID_PARAMETER);
  GlinkSendStatus=EFI_SUCCESS;
  assert(PmicSend(&mPmicProtocol,32778,&SubscriberIdentity,Packet,sizeof(Packet))==EFI_SUCCESS&&GlinkSends==1&&WorkQueued);
  Receive(NULL,Packet,24);assert(SubscriberPackets==1&&GlinkSends==1);
  Packet[0]=0x0b;Receive(NULL,Packet,24);assert(SubscriberPackets==1);Packet[0]=0x0a;
  mQrtr.PdGeneration+=2;Tick();assert(SubscriberState.Online&&SubscriberState.PeerGeneration!=Generation);
  PilRunning=FALSE;assert(GetState(&mProtocol,&State)==EFI_NOT_READY&&!SubscriberState.Online);
  assert(PmicSend(&mPmicProtocol,32778,&SubscriberIdentity,Packet,24)==EFI_NOT_READY);
  PilRunning=TRUE;Tick();assert(SubscriberState.Online&&SubscriberState.AdspGeneration==2);
  IrqStopStatus=EFI_DEVICE_ERROR;
  assert(ClientCallback(ClientContext)==EFI_DEVICE_ERROR&&!SubscriberState.Online&&mIpcc.Registered);
  Receive(NULL,Packet,24);assert(SubscriberPackets==1);
  assert(PmicUnregister(&mPmicProtocol,32778,&mProtocol)==EFI_ACCESS_DENIED);
  assert(PmicUnregister(&mPmicProtocol,32778,&SubscriberIdentity)==EFI_SUCCESS);
  IrqStopStatus=EFI_SUCCESS;assert(UCSICrUnload((EFI_HANDLE)0x1234)==EFI_SUCCESS);
  puts("UCSI DXE: IRQ-driven lifecycle, retained failures and explicit PIL shutdown barrier in both EBS orders PASS");
  return 0;
}
