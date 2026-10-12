/* Actual DXE adapter + portable cold-init, with provider/firmware boundaries.
 * Catches fake source ownership, wrong SID/identity/window, unsafe release of
 * an attempted stream, and EBS stopping hardware before child freeze drains.
 * SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#ifndef CR_HDK_USB_DXE_SOURCE
#define CR_HDK_USB_DXE_SOURCE "../Driver/HdkUsbCrDxe/HdkUsbCrDxe.c"
#endif
#include CR_HDK_USB_DXE_SOURCE
#ifdef CR_HDK_PRIMARY_TEST
#include "PmicCcHostFixture.h"
#define TEST_BASE 0xa600000U
#define TEST_HS 0x88e3000U
#define TEST_SID 0U
#define TEST_GDSC "usb30_prim_gdsc"
#define TEST_RESET "gcc_usb30_prim_bcr"
#define TEST_MEM 0x6000U
#else
#define TEST_BASE 0xa800000U
#define TEST_HS 0x88e4000U
#define TEST_SID 0x20U
#define TEST_GDSC "usb30_sec_gdsc"
#define TEST_RESET "gcc_usb30_sec_bcr"
#define TEST_MEM 0x6004U
#endif
EFI_GUID gEfiClockCrProtocolGuid,gEfiRpmhCrProtocolGuid,gEfiGpioCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid,gEfiSmmuCrProtocolGuid,gEfiUsbTypeCCrProtocolGuid;
EFI_GUID gEdkiiIoMmuProtocolGuid,gEfiDmaIdentityCrProtocolGuid,gEfiDevicePathProtocolGuid;
EFI_GUID gEdkiiNonDiscoverableDeviceProtocolGuid,gEdkiiUsb2HcFreezeProtocolGuid;
EFI_GUID gEfiUsb2HcProtocolGuid,gEfiEventExitBootServicesGuid;
static EFI_BOOT_SERVICES Bs;
EFI_BOOT_SERVICES *gBS=&Bs;
static EFI_CLOCK_CR_PROTOCOL ClockProvider;
static EFI_RPMH_CR_PROTOCOL RpmhProvider;
static EFI_GPIO_CR_PROTOCOL GpioProvider;
static EFI_INTERCONNECT_CR_PROTOCOL IcbProvider;
static EFI_SMMU_CR_PROTOCOL SmmuProvider;
static EFI_USB2_HC_PROTOCOL HostController;
static EDKII_USB2_HC_FREEZE_PROTOCOL HostFreezer;
static EFI_SMMU_CR_EXIT_PREPARE Prepare;
static VOID *PrepareContext;
static UINT32 C[0x100000/4],H[0x114/4];
static UINT32 ClockRefs,RailRefs,PathRefs,PinRefs,Reads,Writes,Releases,AttachCalls,Publishes,Active;
static BOOLEAN DomainRef,FloorFail,AttachFail,ResetFail,ObserverFail;
static BOOLEAN Reenter,UnloadInside;
static UINT32 ExitInside,ConnectCalls,HandleCalls;
static BOOLEAN PrepareReenter;
#ifdef CR_HDK_PRIMARY_TEST
static EFI_USB_TYPEC_CR_PROTOCOL TypeCProvider;
static UINT32 TypeCOpens,EudValue;
static UINT32 LoseRouteInside;
static VOID LoseRoute(UINT32 Phase){if(LoseRouteInside==Phase){LoseRouteInside=0;CcIrq(CcIrqContext);Monitor(NULL,&mUsb);}}
static EFI_STATUS EFIAPI WitnessMock(EFI_USB_TYPEC_CR_PROTOCOL *P,CR_TYPEC_NON_PD_HOST_WITNESS *W){assert(P==&TypeCProvider&&TypeCOpens==1);return CrPmicCcHostWitness(&Cc,W);}
static EFI_STATUS EFIAPI LocateHandles(EFI_LOCATE_SEARCH_TYPE Type,EFI_GUID *Guid,VOID *Registration,UINTN *N,EFI_HANDLE **Handles){
 static EFI_HANDLE Handle=(VOID *)5;assert(Type==ByProtocol&&Guid==&gEfiUsbTypeCCrProtocolGuid&&!Registration);*N=1;*Handles=&Handle;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE Handle,EFI_GUID *Guid,VOID **Out,EFI_HANDLE Agent,EFI_HANDLE Controller,UINT32 Attr){
 assert(Handle==(VOID *)5&&Guid==&gEfiUsbTypeCCrProtocolGuid&&Agent==(VOID *)4&&Controller==Handle&&Attr==EFI_OPEN_PROTOCOL_BY_DRIVER);
 *Out=&TypeCProvider;++TypeCOpens;return EFI_SUCCESS;
}
VOID EFIAPI FreePool(VOID *P){assert(P);}
static EFI_STATUS EFIAPI EventCreate(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,VOID *Ctx,EFI_EVENT *E){assert(Type==(EVT_TIMER|EVT_NOTIFY_SIGNAL)&&Tpl==TPL_CALLBACK&&Notify==Monitor&&Ctx==&mUsb);*E=(VOID *)6;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Timer(EFI_EVENT E,EFI_TIMER_DELAY Type,UINT64 N){assert(E==(VOID *)6&&Type==TimerPeriodic&&N==100000);return EFI_SUCCESS;}
#endif
static VOID TriggerExit(UINT32 Phase){if(ExitInside==Phase){ExitInside=0;assert(Prepare);(VOID)Prepare(PrepareContext);}}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **P){
 (VOID)Registration;
 if(Reenter){Reenter=FALSE;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_ALREADY_STARTED);}
 if(G==&gEfiClockCrProtocolGuid)*P=&ClockProvider;
 else if(G==&gEfiRpmhCrProtocolGuid)*P=&RpmhProvider;
 else if(G==&gEfiGpioCrProtocolGuid)*P=&GpioProvider;
 else if(G==&gEfiInterconnectCrProtocolGuid)*P=&IcbProvider;
 else if(G==&gEfiSmmuCrProtocolGuid)*P=&SmmuProvider;
 else return EFI_NOT_FOUND;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI FloorMock(EFI_RPMH_CR_PROTOCOL *P){assert(P==&RpmhProvider);return FloorFail?EFI_DEVICE_ERROR:EFI_SUCCESS;}
static EFI_STATUS EFIAPI AcquireClock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *Ctl,CONST CHAR8 *Name,UINT64 Rate,BOOLEAN *O){
 assert(P==&ClockProvider&&!strcmp(Ctl,"gcc")&&Name&&O);(VOID)Rate;
 if(UnloadInside){UnloadInside=FALSE;assert(HdkUsbCrUnload(NULL)==EFI_ACCESS_DENIED);}
 ++ClockRefs;*O=TRUE;
#ifdef CR_HDK_PRIMARY_TEST
 LoseRoute(1);
#endif
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetClock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *Ctl,CONST CHAR8 *Name,UINT64 Rate,BOOLEAN On){
 assert(P==&ClockProvider&&!strcmp(Ctl,"gcc")&&Name&&!On&&!Rate&&ClockRefs);--ClockRefs;++Releases;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AcquireDomain(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *Ctl,CONST CHAR8 *N,BOOLEAN *O){
 assert(P==&ClockProvider&&!strcmp(Ctl,"gcc")&&!strcmp(N,TEST_GDSC)&&!DomainRef);DomainRef=*O=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI SetDomain(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *Ctl,CONST CHAR8 *N,BOOLEAN On){
 assert(P==&ClockProvider&&!strcmp(Ctl,"gcc")&&!strcmp(N,TEST_GDSC)&&!On&&DomainRef&&!ClockRefs);DomainRef=FALSE;++Releases;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ResetMock(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *Ctl,CONST CHAR8 *N,BOOLEAN Assert){
 assert(P==&ClockProvider&&!strcmp(Ctl,"gcc")&&ClockRefs&&DomainRef);
 if(ResetFail)return EFI_DEVICE_ERROR;
 if(Assert&&!strcmp(N,TEST_RESET)){C[0x40/4]=0;C[0x44/4]=1;}
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AcquireRail(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *N,UINT32 Mv,UINT8 Mode,BOOLEAN *O){
 assert(P==&RpmhProvider&&N&&Mv&&Mode==7);++RailRefs;*O=TRUE;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI RailOff(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *N,BOOLEAN On){assert(P==&RpmhProvider&&N&&!On&&RailRefs);--RailRefs;++Releases;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI AcquirePath(EFI_INTERCONNECT_CR_PROTOCOL *P,CONST CHAR8 *N,UINT32 A,UINT32 B,INTERCONNECT_PATH_HANDLE *O){
 assert(P==&IcbProvider&&N&&((A==TEST_MEM&&B==TEST_MEM+1)||(A==TEST_MEM+2&&B==TEST_MEM+3)));*O=++PathRefs;TriggerExit(6);
#ifdef CR_HDK_PRIMARY_TEST
 LoseRoute(2);
#endif
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE Handle,UINT64 Avg,UINT64 Peak){
 assert(!mExiting&&P==&IcbProvider&&Handle&&(Avg==240000||Avg==0)&&(Peak==700000||Peak==40000));
#ifdef CR_HDK_PRIMARY_TEST
 assert(CrPmicCcHostWitness(&Cc,&(CR_TYPEC_NON_PD_HOST_WITNESS){0})==CR_SUCCESS);
#endif
 return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PathOff(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE Handle){assert(P==&IcbProvider&&Handle&&PathRefs);--PathRefs;++Releases;return EFI_SUCCESS;}
static UINT8 PinValue;
static EFI_STATUS EFIAPI PinAcquire(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,CONST GpioConfigParams *Cfg,BOOLEAN *O){
 assert(P==&GpioProvider&&Owner==&mUsb&&Cfg->FunctionSel==0&&Cfg->OutputEnable&&Cfg->DriveStrength==0);
 assert(Cfg->PinNumber==(mUsb.Target->Soc==0x8250?22:42));++PinRefs;PinValue=Cfg->OutputValue;*O=TRUE;TriggerExit(7);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI PinSet(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,UINT16 Pin,UINT8 Value){assert(!mExiting&&P==&GpioProvider&&Owner==&mUsb&&PinRefs&&Pin<300);PinValue=Value;TriggerExit(8);return EFI_SUCCESS;}
static EFI_STATUS EFIAPI PinRead(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,UINT16 Pin,UINT8 *Value){assert(!mExiting&&P==&GpioProvider&&Owner==&mUsb&&PinRefs&&Pin<300);*Value=PinValue;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI PinOff(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,UINT16 Pin){assert(P==&GpioProvider&&Owner==&mUsb&&PinRefs&&Pin<300);--PinRefs;++Releases;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI AcquireStream(EFI_SMMU_CR_PROTOCOL *P,UINT64 Base,UINT16 Sid,EFI_SMMU_CR_USB_VERIFY_STOPPED Verify,VOID *Ctx){
 assert(P==&SmmuProvider&&Base==TEST_BASE&&Sid==TEST_SID&&ClockRefs==6&&RailRefs==3&&DomainRef&&!Publishes);
 ++AttachCalls;assert(Verify(Ctx)==EFI_SUCCESS&&Verify(Ctx)==EFI_SUCCESS);return AttachFail?EFI_DEVICE_ERROR:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI RegisterPrepare(EFI_SMMU_CR_PROTOCOL *P,VOID *Owner,EFI_SMMU_CR_EXIT_PREPARE F,VOID *Ctx){
 assert(P==&SmmuProvider&&Owner==&mUsb);if(ObserverFail)return EFI_DEVICE_ERROR;Prepare=F;PrepareContext=Ctx;TriggerExit(1);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateEx(UINT32 Type,EFI_TPL Tpl,EFI_EVENT_NOTIFY Notify,CONST VOID *Ctx,CONST EFI_GUID *Guid,EFI_EVENT *E){
 assert(Type==EVT_NOTIFY_SIGNAL&&Tpl==TPL_NOTIFY&&Notify==Exit&&Ctx==&mUsb&&Guid==&gEfiEventExitBootServicesGuid);*E=(VOID *)3;TriggerExit(2);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...){
 va_list Ap;va_start(Ap,Handle);EFI_GUID *G=va_arg(Ap,EFI_GUID *);EFI_DMA_IDENTITY_CR_PROTOCOL *Id=va_arg(Ap,VOID *);
 assert(G==&gEfiDmaIdentityCrProtocolGuid&&Id->IommuBase==0x15000000&&Id->ControllerBase==TEST_BASE&&Id->ControllerSize==0xcd00&&Id->Sid==TEST_SID);
 assert(va_arg(Ap,VOID *)==NULL);va_end(Ap);*Handle=(VOID *)1;TriggerExit(3);return EFI_SUCCESS;
}
EFI_STATUS EFIAPI RegisterNonDiscoverableMmioDevice(UINTN Id,NON_DISCOVERABLE_DEVICE_TYPE Type,NON_DISCOVERABLE_DEVICE_DMA_TYPE Dma,
 NON_DISCOVERABLE_DEVICE_INIT Init,EFI_HANDLE *Handle,UINTN Count,...){
 assert(Id==(TEST_SID?0xd301:0xd300)&&Type==NonDiscoverableDeviceTypeXhci&&Dma==NonDiscoverableDeviceDmaTypeNonCoherent&&Count==1&&*Handle==(VOID *)1&&Prepare);
 assert(!mExiting);va_list Ap;va_start(Ap,Count);assert(va_arg(Ap,UINTN)==TEST_BASE&&va_arg(Ap,UINTN)==0xc000);va_end(Ap);assert(Init(NULL)==EFI_SUCCESS);++Publishes;TriggerExit(4);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE Handle,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *Remaining,BOOLEAN Recurse){
 assert(Handle==(VOID *)1&&!Drivers&&!Remaining&&Recurse&&mUsb.Ready);++ConnectCalls;TriggerExit(5);
#ifdef CR_HDK_PRIMARY_TEST
 LoseRoute(3);if(AdmitHost(&mUsb)!=EFI_SUCCESS){assert(HostInit(NULL)==EFI_NOT_READY);return EFI_ACCESS_DENIED;}
#endif
 if(mExiting){assert(HostInit(NULL)==EFI_NOT_READY);return EFI_ACCESS_DENIED;}
 C[0x40/4]=5;C[0x44/4]=0;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI HostFreezeMock(EDKII_USB2_HC_FREEZE_PROTOCOL *F,UINT32 *N){
 assert(F==&HostFreezer);if(PrepareReenter){PrepareReenter=FALSE;assert(Prepare(PrepareContext)==EFI_NOT_READY);}
 *N=Active;return Active?EFI_NOT_READY:EFI_SUCCESS;
}
static EFI_STATUS EFIAPI HandleProtocol(EFI_HANDLE Handle,EFI_GUID *G,VOID **P){
 assert(Handle==(VOID *)1);++HandleCalls;if(G==&gEdkiiUsb2HcFreezeProtocolGuid)*P=&HostFreezer;else if(G==&gEfiUsb2HcProtocolGuid)*P=&HostController;else return EFI_NOT_FOUND;return EFI_SUCCESS;
}
UINT32 EFIAPI MmioRead32(UINTN A){
#ifdef CR_HDK_PRIMARY_TEST
 if(A==0x88e2000){++Reads;assert(!FloorFail);return EudValue;}
#endif
 ++Reads;assert(ClockRefs&&DomainRef&&!FloorFail);
 if(A>=TEST_BASE&&A<TEST_BASE+0x100000)return C[(A-TEST_BASE)/4];
 assert(A>=TEST_HS&&A<TEST_HS+0x114&&RailRefs==3);return H[(A-TEST_HS)/4];
}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){
 ++Writes;assert(ClockRefs&&DomainRef);
 if(A>=TEST_BASE&&A<TEST_BASE+0x100000){UINT32 O=(UINT32)(A-TEST_BASE);if(O==0xc40c)C[O/4]-=V&0xfffc;else C[O/4]=V;if(O==0x40&&!(V&1))C[0x44/4]|=1;}
 else {assert(A>=TEST_HS&&A<TEST_HS+0x114&&RailRefs==3);H[(A-TEST_HS)/4]=V;}return V;
}
VOID EFIAPI MemoryFence(VOID){}
UINTN EFIAPI MicroSecondDelay(UINTN Us){return Us;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}
static VOID Fresh(VOID){
 memset(&mUsb,0,sizeof mUsb);memset(C,0,sizeof C);memset(H,0xa5,sizeof H);C[0]=0x40;C[0x44/4]=1;
 C[0xc120/4]=0x33313130;C[0xc140/4]=2;C[0xc1a0/4]=0x3139302a;
 mHost=NULL;mExit=NULL;mStarting=mHostRegistered=mStreamAttempted=mObserver=mExiting=FALSE;memset(mPaths,0,sizeof mPaths);
#ifndef CR_HDK_USB_OLD_DXE
 mEntered=mEntryBusy=mRetiring=mExitFrozen=mConnecting=mConnectAttempted=mPreparing=FALSE;
#endif
 ClockRefs=RailRefs=PathRefs=PinRefs=Reads=Writes=Releases=AttachCalls=Publishes=Active=ConnectCalls=HandleCalls=ExitInside=0;DomainRef=FloorFail=AttachFail=ResetFail=ObserverFail=FALSE;Prepare=NULL;
 Bs=(EFI_BOOT_SERVICES){.LocateProtocol=Locate,.CreateEventEx=CreateEx,.InstallMultipleProtocolInterfaces=Install,.ConnectController=Connect,.HandleProtocol=HandleProtocol};
 ClockProvider=(EFI_CLOCK_CR_PROTOCOL){.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.AcquireClock=AcquireClock,.SetClock=SetClock,.AcquireGdsc=AcquireDomain,.SetGdsc=SetDomain,.SetReset=ResetMock};
 RpmhProvider=(EFI_RPMH_CR_PROTOCOL){.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.EnsureBootFloor=FloorMock,.AcquireVreg=AcquireRail,.RpmhEnableVreg=RailOff};
 GpioProvider=(EFI_GPIO_CR_PROTOCOL){.Revision=0x10002,.AcquirePin=PinAcquire,.SetPinValue=PinSet,.ReadPinValue=PinRead,.ReleasePin=PinOff};
 IcbProvider=(EFI_INTERCONNECT_CR_PROTOCOL){.AcquirePath=AcquirePath,.SetBandwidth=Bandwidth,.ReleasePath=PathOff};
 SmmuProvider=(EFI_SMMU_CR_PROTOCOL){.Revision=0x1000a,.AcquireUsbStream=AcquireStream,.SetExitPrepareObserver=RegisterPrepare};
 HostFreezer=(EDKII_USB2_HC_FREEZE_PROTOCOL){EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION,&HostController,HostFreezeMock};
#ifdef CR_HDK_PRIMARY_TEST
 CcFresh();mTypeC=NULL;mImage=mTypeHandle=NULL;mMonitor=NULL;mTypeLease=mMonitorBusy=FALSE;mRouteEpoch=mAttemptEpoch=mReportedEpoch=0;TypeCOpens=EudValue=LoseRouteInside=0;
 TypeCProvider=(EFI_USB_TYPEC_CR_PROTOCOL){.Revision=EFI_USB_TYPEC_NON_PD_WITNESS_REVISION,.GetNonPdHostWitness=WitnessMock};
 Bs.LocateHandleBuffer=LocateHandles;Bs.OpenProtocol=Open;Bs.CreateEvent=EventCreate;Bs.SetTimer=Timer;
#endif
}
int main(int Count,char **Arguments){
#ifdef CR_HDK_PRIMARY_TEST
 (VOID)Count;(VOID)Arguments;
 Fresh();UINT32 CcBefore=CcReads;assert(HdkUsbCrEntryPoint((VOID *)4,NULL)==EFI_SUCCESS&&Publishes==1&&AttachCalls==1&&TypeCOpens==1&&!PinRefs&&CcReads==CcBefore&&!Cc.State.PdReady);
 assert(H[0x6c/4]==0xc7&&H[0x70/4]==3&&H[0x74/4]==3);
 UINT32 HeldClocks=ClockRefs;CcIrq(CcIrqContext);Monitor(NULL,&mUsb);
 assert(mExiting&&!(C[0x40/4]&5)&&(C[0x44/4]&1)&&ClockRefs==HeldClocks&&!Releases);
 assert(HdkUsbCrUnload(NULL)==EFI_ACCESS_DENIED);assert(CrPmicCcService(&Cc)==CR_SUCCESS);Monitor(NULL,&mUsb);assert(Publishes==1);
 for(UINT32 Case=0;Case<3;Case++){
  Fresh();if(Case==0)CcRaw[7]=1;if(Case==1)CcRaw[4]=0;if(Case==2)EudValue=1;
  CcSample();assert(HdkUsbCrEntryPoint((VOID *)4,NULL)==EFI_SUCCESS&&!Publishes&&!AttachCalls&&!ClockRefs&&!Writes);
 }
 Fresh();CcRaw[4]=0;CcSample();assert(HdkUsbCrEntryPoint((VOID *)4,NULL)==EFI_SUCCESS&&!Publishes);
 CcRaw[4]=0x20;CcSample();Monitor(NULL,&mUsb);assert(Publishes==1&&AttachCalls==1);
 for(UINT32 Phase=1;Phase<=2;Phase++){
  Fresh();LoseRouteInside=Phase;assert(HdkUsbCrEntryPoint((VOID *)4,NULL)==EFI_SUCCESS&&!Publishes&&!AttachCalls&&!ClockRefs&&!PathRefs);
 }
 Fresh();LoseRouteInside=3;assert(HdkUsbCrEntryPoint((VOID *)4,NULL)==EFI_SUCCESS&&Publishes==1&&AttachCalls==1);
 Monitor(NULL,&mUsb);assert(mExiting&&!(C[0x40/4]&5)&&ClockRefs==6&&!Releases);
 for(UINT32 Phase=1;Phase<=6;Phase++){
  Fresh();ExitInside=Phase;assert(HdkUsbCrEntryPoint((VOID *)4,NULL)==EFI_SUCCESS&&mExiting&&!Releases);
  assert(ConnectCalls==(Phase==5?1U:0U));assert(Publishes==(Phase==4||Phase==5?1U:0U));
 }
 puts("HDK8245 actual PM8150B/native USB0: pure retained non-PD source/VBUS epoch, EUD gate, first qualified attach, IRQ freeze/halt and no restart PASS");return 0;
#else
 if(Count>1&&!strcmp(Arguments[1],"entry")){Fresh();Reenter=TRUE;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS);return 0;}
 if(Count>1&&!strcmp(Arguments[1],"unload")){Fresh();UnloadInside=TRUE;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS);return 0;}
 if(Count>1&&!strcmp(Arguments[1],"exit")){
  for(UINT32 Phase=1;Phase<=5;Phase++){
   Fresh();ExitInside=Phase;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&mExiting);
   assert(ConnectCalls==(Phase==5?1U:0U));assert(Publishes==(Phase>=4?1U:0U));
   assert(!(C[0x40/4]&5)&&!Releases);assert(HdkUsbCrUnload(NULL)==EFI_ACCESS_DENIED);
  }return 0;
 }
 if(Count>1&&!strcmp(Arguments[1],"provider-exit")){
  for(UINT32 Phase=6;Phase<=8;Phase++){
   Fresh();ExitInside=Phase;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&mExiting);
   assert(!ConnectCalls&&!Publishes&&!Releases);assert(HdkUsbCrUnload(NULL)==EFI_ACCESS_DENIED);
  }return 0;
 }
 Fresh();FloorFail=TRUE;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&!Reads&&!Writes&&!Publishes&&!ClockRefs&&mObserver);
 Fresh();SmmuProvider.Revision=0x10009;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_UNSUPPORTED&&!Reads&&!Writes);
 Fresh();AttachFail=TRUE;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&!Publishes&&mStreamAttempted&&ClockRefs==6&&RailRefs==3&&DomainRef&&!Releases);
 assert(HdkUsbCrUnload(NULL)==EFI_ACCESS_DENIED);
 Fresh();ObserverFail=TRUE;assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_DEVICE_ERROR&&!Publishes&&!mStreamAttempted&&!ClockRefs&&!Releases);
 Fresh();assert(HdkUsbCrEntryPoint(NULL,NULL)==EFI_SUCCESS&&Publishes==1&&AttachCalls==1&&PinRefs==1&&Prepare);
 assert(PinValue==(mUsb.Target->Soc==0x8250?1:0));
 Active=1;UINT32 Before=Writes;assert(Prepare(PrepareContext)==EFI_NOT_READY&&Writes==Before&&C[0x40/4]==5);
 Active=0;PrepareReenter=TRUE;assert(Prepare(PrepareContext)==EFI_SUCCESS&&!(C[0x40/4]&5)&&(C[0x44/4]&1)&&!Releases&&ClockRefs==6&&RailRefs==3&&PinRefs==1);
 assert(HdkUsbCrUnload(NULL)==EFI_ACCESS_DENIED);UINT32 HandleBefore=HandleCalls,WriteBefore=Writes;
 assert(Prepare(PrepareContext)==EFI_SUCCESS&&HandleCalls==HandleBefore&&Writes==WriteBefore);
 puts("HDK USB actual DXE: source floor, cold controller, stopped SID0x20, exact identity/child, retained stream failure and freeze-before-revocation PASS");return 0;
#endif
}
