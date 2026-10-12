/** Production role monitor and rollback fixtures. SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Driver/Dwc3CrDxe/Dwc3CrDxe.c"
#ifdef CR_DWC3_NATIVE_CLOCK_TEST
#include "Dwc3ClockTest.h"
#endif

EFI_GUID gEfiUsbTypeCCrProtocolGuid, gEfiUsbDpPhyCrProtocolGuid, gEfiClockCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid, gEfiSmmuCrProtocolGuid, gEdkiiIoMmuProtocolGuid;
EFI_GUID gEfiDmaIdentityCrProtocolGuid, gEfiDevicePathProtocolGuid, gEfiUsbFunctionIoProtocolGuid;
EFI_GUID gEdkiiNonDiscoverableDeviceProtocolGuid, gEfiEventExitBootServicesGuid;
EFI_GUID gEdkiiUsb2HcFreezeProtocolGuid, gEfiUsb2HcProtocolGuid;
static EFI_USB2_HC_PROTOCOL HostHc;
static EDKII_USB2_HC_FREEZE_PROTOCOL HostFreeze;
static CR_USB_NATIVE_FREEZE FreezeProducer;
static CR_USB_NATIVE_HANDOFF Record;
static UINT32 FreezeActive;
static EFI_STATUS FreezeResult=EFI_SUCCESS;
EFI_USBFN_IO_PROTOCOL gCrUsbFunctionIo;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static EFI_USB_TYPEC_STATE State;
static EFI_USB_TYPEC_CR_PROTOCOL TypeC;
static BOOLEAN TypeCPublished;
static EFI_USB_DP_PHY_CR_PROTOCOL Phy;
static EFI_CLOCK_CR_PROTOCOL Clock;
static EFI_INTERCONNECT_CR_PROTOCOL Icb;
static EFI_SMMU_CR_PROTOCOL Smmu;
static EDKII_IOMMU_PROTOCOL Iommu;
static NON_DISCOVERABLE_DEVICE *HostDevice;
static EFI_DEVICE_PATH_PROTOCOL *HostPath;
static EFI_STATUS AttachStatus, HaltStatus, ConnectStatus, TimerStatus;
static UINTN ClockVotes, PathVotes, CoreStarts, Halts, FnPolls, Disconnects, Releases, Creates, FailCreate;
static BOOLEAN DomainVote, PhyVote, FnAvailable, FailPhyRelease;
static EFI_EVENT_NOTIFY TimerCallback, ExitCallback;

VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
VOID EFIAPI FreePool(VOID *P) { free(P); }
UINTN EFIAPI MicroSecondDelay(UINTN N) { return N; }
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
EFI_STATUS CrUsbCoreInit(EFI_USB_TYPEC_DATA_ROLE R, BOOLEAN Ss)
{ assert((R==UsbTypeCHost || R==UsbTypeCDevice) && DomainVote && PhyVote && (R==UsbTypeCHost || !Ss)); ++CoreStarts; return EFI_SUCCESS; }
EFI_STATUS CrUsbCoreHalt(EFI_USB_TYPEC_DATA_ROLE R)
{ assert(R!=UsbTypeCNone && ClockVotes==6); ++Halts; return HaltStatus; }
EFI_STATUS CrUsbCoreRetire(EFI_USB_TYPEC_DATA_ROLE R,CR_USB_CONTROLLER_STOP *Stop,EFI_STATUS *Halt,EFI_STATUS *Interrupt) {
 *Halt=CrUsbCoreHalt(R);*Interrupt=EFI_SUCCESS;
 *Stop=(CR_USB_CONTROLLER_STOP){.Role=(UINT32)R,.Id=0x33313130,.GlobalControl=(UINT32)R<<12,.EventBuffers=1,.MaskedEventBuffers=1};
 return *Halt;
}
EFI_STATUS CrUsbFnFreeze(UINT32 *Active) { *Active=0;FnAvailable=FALSE;return EFI_SUCCESS; }
EFI_STATUS CrUsbNativeInit(EFI_SMMU_CR_PROTOCOL *P,CR_USB_NATIVE_FREEZE F){ assert(P==&Smmu);FreezeProducer=F;return EFI_SUCCESS; }
EFI_STATUS CrUsbNativeRelease(VOID){FreezeProducer=NULL;return EFI_SUCCESS;}
VOID CrUsbNativeExit(VOID){assert(FreezeProducer);memset(&Record,0,sizeof(Record));Record.FreezeStatus=FreezeProducer(&Record);}
static EFI_STATUS EFIAPI FreezeHost(EDKII_USB2_HC_FREEZE_PROTOCOL *P,UINT32 *Active){assert(P==&HostFreeze);*Active=FreezeActive;return FreezeResult;}
VOID CrUsbFnInit(EDKII_IOMMU_PROTOCOL *I) { assert(I==&Iommu); }
VOID CrUsbFnSetAvailable(BOOLEAN A) { FnAvailable=A; }
EFI_STATUS CrUsbFnPoll(VOID) { assert(FnAvailable); ++FnPolls; return EFI_SUCCESS; }
EFI_STATUS CrUsbFnStop(VOID) { assert(!FnAvailable); return EFI_SUCCESS; }
BOOLEAN CrUsbFnCanUnload(VOID) { return TRUE; }
static EFI_STATUS EFIAPI GetState(EFI_USB_TYPEC_CR_PROTOCOL *T, EFI_USB_TYPEC_STATE *S)
{ assert(T==&TypeC && TypeCPublished); *S=State; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI AcquirePhy(EFI_USB_DP_PHY_CR_PROTOCOL *P, EFI_USB_DP_PHY_CLIENT C,
  CONST EFI_USB_TYPEC_STATE *S, BOOLEAN Ss, BOOLEAN *Owned)
{ assert(P==&Phy && C==UsbDpPhyUsb && S->Connected && !PhyVote && (S->DataRole==UsbTypeCHost || !Ss)); PhyVote=*Owned=TRUE; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ReleasePhy(EFI_USB_DP_PHY_CR_PROTOCOL *P, EFI_USB_DP_PHY_CLIENT C)
{ assert(P==&Phy && C==UsbDpPhyUsb && PhyVote && Halts); if(FailPhyRelease) return EFI_DEVICE_ERROR; PhyVote=FALSE; ++Releases; return EFI_SUCCESS; }
static BOOLEAN FailClockAcquire, FailClockRelease;
static EFI_STATUS EFIAPI SetClock(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN Enable)
{
  assert(P==&Clock && strcmp(C,"gcc")==0 && N);
  if (!Enable && FailClockRelease) return EFI_TIMEOUT;
#ifdef CR_DWC3_NATIVE_CLOCK_TEST
  EFI_STATUS Status=UsbNativeClock(N,R,Enable);
  if(EFI_ERROR(Status)) return Status;
#endif
  if(Enable) ++ClockVotes;
  else { assert(ClockVotes && R==0 && !PhyVote); --ClockVotes; }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Gdsc(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN E)
{
  assert(P==&Clock && strcmp(C,"gcc")==0 && strcmp(N,"usb30_prim_gdsc")==0 && ClockVotes==0);
#ifdef CR_DWC3_NATIVE_CLOCK_TEST
  /* A held wrapper reset prevents a cold domain from powering up.
     Domain acquisition cannot implicitly clear a hardware reset. */
  if(E && UsbClockCoreReset) return EFI_TIMEOUT;
#endif
  DomainVote=E;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AcquireClockMock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN *Owned)
{
  EFI_STATUS Status = SetClock(P, C, N, R, TRUE);
  *Owned = !EFI_ERROR(Status);
  if (FailClockAcquire && !EFI_ERROR(Status)) return EFI_TIMEOUT;
  return Status;
}
static EFI_STATUS EFIAPI AcquireGdscMock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN *Owned)
{
  EFI_STATUS Status = Gdsc(P, C, N, TRUE);
  *Owned = !EFI_ERROR(Status);
  return Status;
}
static EFI_STATUS EFIAPI Reset(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, BOOLEAN E)
{
  assert(P==&Clock && strcmp(C,"gcc")==0 && strcmp(N,"gcc_usb30_prim_bcr")==0 &&
         (ClockVotes<=6));
#ifdef CR_DWC3_NATIVE_CLOCK_TEST
  UsbClockCoreReset=E;
  if(E) UsbClockRegisters[GccUsbMock.EnableRegister / 4U] |= BIT31;
#else
  (VOID)E;
#endif
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Divider(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, UINT32 D)
{
  assert(P==&Clock && C && strcmp(N,"gcc_usb30_prim_mock_utmi_clk")==0 && D==1);
#ifdef CR_DWC3_NATIVE_CLOCK_TEST
  if(UsbClockNode(N)->ReferenceCount != 0) return EFI_ACCESS_DENIED;
  assert((MmioRead32(0x149058U)&0xFU)==0);
#endif
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI AcquirePath(EFI_INTERCONNECT_CR_PROTOCOL *P, CONST CHAR8 *C, UINT32 S, UINT32 D, INTERCONNECT_PATH_HANDLE *H)
{ assert(P==&Icb && strcmp(C,"qcom,sm8450-dwc3")==0 && D==S+1 && (S==0x6000 || S==0x6002)); *H=++PathVotes; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *P, INTERCONNECT_PATH_HANDLE H, UINT64 A, UINT64 B)
{ assert(P==&Icb && H && B && (H==1 || A==0)); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ReleasePath(EFI_INTERCONNECT_CR_PROTOCOL *P, INTERCONNECT_PATH_HANDLE H)
{ assert(P==&Icb && H && PathVotes && !DomainVote); --PathVotes; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Attach(EFI_SMMU_CR_PROTOCOL *P, UINT16 Segment, UINT16 Bdf, UINT16 Sid)
{ assert(P==&Smmu && Segment==0 && Bdf==0xFFFF && Sid==0); return AttachStatus; }
static EFI_STATUS EFIAPI Publish(EFI_HANDLE *H, ...)
{
  va_list A; va_start(A,H); EFI_GUID *G=va_arg(A,EFI_GUID *);
  if(G==&gEfiDmaIdentityCrProtocolGuid) { assert(va_arg(A,VOID *)==&mIdentity); *H=(EFI_HANDLE)0xD0; }
  else { assert(G==&gEfiUsbFunctionIoProtocolGuid && va_arg(A,VOID *)==&gCrUsbFunctionIo); *H=(EFI_HANDLE)0xF0; }
  va_end(A); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Remove(EFI_HANDLE H, ...)
{
  va_list A; va_start(A,H); EFI_GUID *G=va_arg(A,EFI_GUID *);
  assert(H==(EFI_HANDLE)0xD0 || H==(EFI_HANDLE)0xF0);
  if(G==&gEdkiiNonDiscoverableDeviceProtocolGuid) { assert(va_arg(A,VOID *)==HostDevice); HostDevice=NULL; HostPath=NULL; }
  else assert(G==&gEfiDmaIdentityCrProtocolGuid || G==&gEfiUsbFunctionIoProtocolGuid);
  va_end(A); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H, EFI_GUID *G, VOID **P)
{
  if(G==&gEdkiiUsb2HcFreezeProtocolGuid){*P=&HostFreeze;return EFI_SUCCESS;}
  if(G==&gEfiUsb2HcProtocolGuid){*P=&HostHc;return EFI_SUCCESS;}
 assert(H==(EFI_HANDLE)0xD0); *P=G==&gEfiDevicePathProtocolGuid ? (VOID *)HostPath : HostDevice; return EFI_SUCCESS; }
EFI_STATUS EFIAPI RegisterNonDiscoverableMmioDevice(UINTN Id, NON_DISCOVERABLE_DEVICE_TYPE Type,
  NON_DISCOVERABLE_DEVICE_DMA_TYPE Dma, NON_DISCOVERABLE_DEVICE_INIT Init, EFI_HANDLE *H, UINTN N, ...)
{
  va_list A; assert(Id==0xD300 && Type==NonDiscoverableDeviceTypeXhci && Dma==NonDiscoverableDeviceDmaTypeNonCoherent && *H==(EFI_HANDLE)0xD0 && N==1);
  va_start(A,N); assert(va_arg(A,UINTN)==0xA600000 && va_arg(A,UINTN)==0xC000); va_end(A);
  HostDevice=calloc(1,sizeof(*HostDevice)); HostPath=calloc(1,sizeof(*HostPath));
  assert(Init(HostDevice)==EFI_SUCCESS); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE H, EFI_HANDLE *D, EFI_DEVICE_PATH_PROTOCOL *P, BOOLEAN Recursive)
{ assert(H==(EFI_HANDLE)0xD0 && D==NULL && P==NULL && Recursive); return ConnectStatus; }
static EFI_STATUS EFIAPI Disconnect(EFI_HANDLE H, EFI_HANDLE D, EFI_HANDLE Child)
{ assert((H==(EFI_HANDLE)0xD0 || H==(EFI_HANDLE)0xF0) && D==NULL && Child==NULL); if(H==(EFI_HANDLE)0xD0) assert(Halts); ++Disconnects; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Locate(EFI_GUID *G, VOID *R, VOID **P)
{
  assert(R==NULL);
  if(G==&gEfiUsbTypeCCrProtocolGuid && !TypeCPublished) return EFI_NOT_FOUND;
  *P=G==&gEfiUsbTypeCCrProtocolGuid ? (VOID *)&TypeC : G==&gEfiUsbDpPhyCrProtocolGuid ? (VOID *)&Phy :
     G==&gEfiClockCrProtocolGuid ? (VOID *)&Clock : G==&gEfiInterconnectCrProtocolGuid ? (VOID *)&Icb :
     G==&gEfiSmmuCrProtocolGuid ? (VOID *)&Smmu : (VOID *)&Iommu;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CreateTimer(UINT32 T, EFI_TPL L, EFI_EVENT_NOTIFY N, VOID *C, EFI_EVENT *E)
{ assert(T==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && L==TPL_CALLBACK && C==NULL); if(++Creates==FailCreate) return EFI_OUT_OF_RESOURCES; TimerCallback=N; *E=(EFI_EVENT)1; N(*E,C); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI CreateExit(UINT32 T, EFI_TPL L, EFI_EVENT_NOTIFY N, CONST VOID *C, CONST EFI_GUID *G, EFI_EVENT *E)
{ assert(T==EVT_NOTIFY_SIGNAL && L==TPL_CALLBACK && C==NULL && G==&gEfiEventExitBootServicesGuid); if(++Creates==FailCreate) return EFI_OUT_OF_RESOURCES; ExitCallback=N; *E=(EFI_EVENT)2; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Timer(EFI_EVENT E, EFI_TIMER_DELAY D, UINT64 N)
{ assert(E==(EFI_EVENT)1 && (D==TimerCancel ? N==0 : D==TimerPeriodic && N==100000)); return D==TimerCancel ? EFI_SUCCESS : TimerStatus; }
static EFI_STATUS EFIAPI Close(EFI_EVENT E) { assert(E==(EFI_EVENT)1 || E==(EFI_EVENT)2); return EFI_SUCCESS; }
static VOID Fixture(VOID)
{
  assert(!ClockVotes && !PathVotes && !HostDevice && !PhyVote && !DomainVote);
#ifdef CR_DWC3_NATIVE_CLOCK_TEST
  UsbClockFixture();
#endif
  mHost=mFunction=NULL; mTimer=mExit=NULL; mExiting=mBusy=mActive=mCoreReady=mHostRegistered=FALSE;
  memset(&mPower,0,sizeof(mPower));
  mRole=UsbTypeCNone; mRetryTicks=mCleanupRetryTicks=0; mHaveRetryRoute=FALSE;
  State=(EFI_USB_TYPEC_STATE){.PdReady=TRUE,.Connected=TRUE,.DataRole=UsbTypeCHost,.Usb3=TRUE,.Sequence=1};
  Halts=CoreStarts=FnPolls=Disconnects=Releases=Creates=FailCreate=0; FailPhyRelease=FALSE;
  AttachStatus=HaltStatus=ConnectStatus=TimerStatus=EFI_SUCCESS;
  TypeCPublished=TRUE; TypeC.Revision=EFI_USB_TYPEC_CR_PROTOCOL_REVISION;
  TypeC.GetState=GetState; Phy.Acquire=AcquirePhy; Phy.Release=ReleasePhy;
  FailClockAcquire=FailClockRelease=FALSE;
  Clock.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION; Clock.AcquireClock=AcquireClockMock; Clock.AcquireGdsc=AcquireGdscMock;
  Clock.SetClock=SetClock; Clock.SetGdsc=Gdsc; Clock.SetReset=Reset; Clock.SetDivider=Divider;
  Icb.AcquirePath=AcquirePath; Icb.SetBandwidth=Bandwidth; Icb.ReleasePath=ReleasePath; Smmu.Attach=Attach;
  Iommu.Revision=EDKII_IOMMU_PROTOCOL_REVISION;
  /* Entry validates availability; these functions are never called here. */
  Iommu.Map=(EDKII_IOMMU_MAP)(UINTN)1; Iommu.Unmap=(EDKII_IOMMU_UNMAP)(UINTN)1;
  Iommu.AllocateBuffer=(EDKII_IOMMU_ALLOCATE_BUFFER)(UINTN)1; Iommu.FreeBuffer=(EDKII_IOMMU_FREE_BUFFER)(UINTN)1;
  Iommu.SetAttributeById=(EDKII_IOMMU_SET_ATTRIBUTE_BY_ID)(UINTN)1;
  HostFreeze=(EDKII_USB2_HC_FREEZE_PROTOCOL){EDKII_USB2_HC_FREEZE_PROTOCOL_REVISION,&HostHc,FreezeHost};
  FreezeResult=EFI_SUCCESS;FreezeActive=0;
  Services.LocateProtocol=Locate; Services.HandleProtocol=Handle;
  Services.InstallMultipleProtocolInterfaces=Publish; Services.UninstallMultipleProtocolInterfaces=Remove;
  Services.ConnectController=Connect; Services.DisconnectController=Disconnect;
  Services.CreateEvent=CreateTimer; Services.CreateEventEx=CreateExit; Services.SetTimer=Timer; Services.CloseEvent=Close;
}
static VOID RetryCleanup(VOID)
{
  while(mCleanupRetryTicks) TimerCallback((EFI_EVENT)1,NULL);
  TimerCallback((EFI_EVENT)1,NULL);
}
int main(void)
{
  for(UINT32 Bad=0;Bad<3;++Bad) {
    Fixture();
    if(Bad==0)Clock.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION-1;
    else if(Bad==1)Clock.AcquireClock=NULL;
    else Clock.AcquireGdsc=NULL;
    assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_UNSUPPORTED&&!mPower.Initialized&&!mFunction&&!ClockVotes&&!PathVotes);
  }
  for(UINTN F=1;F<=2;++F) { Fixture(); FailCreate=F; assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_OUT_OF_RESOURCES && mFunction==NULL && mTimer==NULL && mExit==NULL); }
  Fixture(); TimerStatus=EFI_DEVICE_ERROR; assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_DEVICE_ERROR && mFunction==NULL);
  Fixture(); assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_ALREADY_STARTED && CoreStarts==0);
  TimerCallback((EFI_EVENT)1,NULL); assert(mActive && mHostRegistered && CoreStarts==1 && !FnAvailable);
  TimerCallback((EFI_EVENT)1,NULL); assert(CoreStarts==1 && FnPolls==0);
  State.DataRole=UsbTypeCDevice; ++State.Sequence;
  TimerCallback((EFI_EVENT)1,NULL); assert(mActive && mHost==NULL && FnAvailable && CoreStarts==2 && Releases==1);
  TimerCallback((EFI_EVENT)1,NULL); assert(FnPolls==1);
  FailPhyRelease=TRUE; State.Connected=FALSE; TimerCallback((EFI_EVENT)1,NULL);
  assert(!mActive && mPhyOwned && ClockVotes==6 && PathVotes==2);
  assert(mCleanupRetryTicks==100);
  UINTN PriorHalts=Halts;
  for(UINTN I=0;I<100;++I) { ++State.Sequence; TimerCallback((EFI_EVENT)1,NULL); }
  assert(Halts==PriorHalts && mPhyOwned && ClockVotes==6);
  FailPhyRelease=FALSE; TimerCallback((EFI_EVENT)1,NULL); assert(!ClockVotes && !PathVotes && !mPhyOwned);
  assert(Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS);
  Fixture(); assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  AttachStatus=EFI_DEVICE_ERROR; HaltStatus=EFI_TIMEOUT;
  TimerCallback((EFI_EVENT)1,NULL); assert(mCoreReady && !mActive && CoreStarts==1);
  UINTN H=Halts; RetryCleanup(); assert(Halts>H && !mActive && CoreStarts==1);
  HaltStatus=AttachStatus=EFI_SUCCESS; RetryCleanup();
  assert(!mCoreReady && !ClockVotes); mRetryTicks=0; TimerCallback((EFI_EVENT)1,NULL); assert(mActive);
  ExitCallback((EFI_EVENT)2,NULL); assert(mExiting && ClockVotes==6 && PathVotes==2 && mHostRegistered);
  H=CoreStarts; TimerCallback((EFI_EVENT)1,NULL); assert(CoreStarts==H && Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED);
  mExiting=FALSE; assert(Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS);
  Fixture(); assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  TimerCallback((EFI_EVENT)1,NULL); assert(mActive && CoreStarts==1);
  TypeCPublished=FALSE; TimerCallback((EFI_EVENT)1,NULL);
  assert(!mActive && mTypeC==NULL && !ClockVotes && !PathVotes && !mPhyOwned);
  TypeCPublished=TRUE; TimerCallback((EFI_EVENT)1,NULL); assert(mActive && CoreStarts==2);
  assert(Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS);
  Fixture(); assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  AttachStatus=EFI_DEVICE_ERROR;
  TimerCallback((EFI_EVENT)1,NULL); assert(!mActive && mRetryTicks==100 && CoreStarts==1);
  ++State.HpdIrqSequence; ++State.Sequence;
  TimerCallback((EFI_EVENT)1,NULL); assert(mRetryTicks==99 && CoreStarts==1);
  AttachStatus=EFI_SUCCESS; State.Orientation=UsbTypeCReverse;
  TimerCallback((EFI_EVENT)1,NULL); assert(mActive && CoreStarts==2);
  assert(Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS);
  Fixture(); assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  FailClockAcquire=FailClockRelease=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);
  assert(!mActive && ClockVotes==1 && mPower.Clocks[0] && mPower.Domain && mCleanupRetryTicks==100);
  FailClockAcquire=FailClockRelease=FALSE;
  RetryCleanup(); assert(!ClockVotes && !mPower.Clocks[0] && !mPower.Domain);
  mRetryTicks=0; TimerCallback((EFI_EVENT)1,NULL); assert(mActive);
  assert(Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS);
  for(UINT32 Case=0;Case<5;++Case){
    Fixture();assert(Dwc3CrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
    TimerCallback((EFI_EVENT)1,NULL);assert(mActive);
    if(Case==0){FreezeActive=1;FreezeResult=EFI_NOT_READY;}
    if(Case==1)FreezeResult=EFI_WARN_UNKNOWN_GLYPH;
    if(Case==2)HostFreeze.HostController=NULL;
    if(Case==3)mBusy=TRUE;
    if(Case==4)HaltStatus=EFI_TIMEOUT;
    H=Halts;ExitCallback((EFI_EVENT)2,NULL);
    assert(mExiting && ClockVotes==6 && PathVotes==2 && mPhyOwned);
    if(Case<4)assert(Record.FreezeStatus!=EFI_SUCCESS && Halts==H);
    else assert(Record.FreezeStatus==EFI_SUCCESS && Record.HaltStatus==EFI_TIMEOUT && Halts==H+1);
    if(Case==0)assert(Record.FrozenWriters==7 && Record.DrainedWriters==5 && Record.ActiveCalls[1]==1);
    if(Case==3)assert(Record.ActiveCalls[0]==1 && (Record.DrainedWriters&1)==0);
    // Simulated firmware reset for fixture-owned native state.
    mBusy=mExiting=FALSE;HaltStatus=EFI_SUCCESS;
    assert(Dwc3CrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS);
  }
  puts("DWC3 roles: Shell polling, Type-C provider unload/reload, role transition, retained cleanup and EBS passed");
  return 0;
}
