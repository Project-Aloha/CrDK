/** DP production Shell monitor, independent GOP and retained rollback fixtures.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <Protocol/DriverBinding.h>
static BOOLEAN ConsolePolicy=TRUE;
#define _PCD_VALUE_PcdPlatformDisplayPortConsoleEnable ConsolePolicy
#include "../Driver/MDSSDpCrDxe/MDSSDpCrDxe.c"
#include "../Driver/MDSSDpCrDxe/DpGop.c"
#include "../Driver/MDSSDpCrDxe/DpReservation.c"

EFI_GUID gEfiUsbTypeCCrProtocolGuid, gEfiUsbDpPhyCrProtocolGuid, gEfiClockCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid, gEfiSmmuCrProtocolGuid, gEdkiiIoMmuProtocolGuid;
EFI_GUID gEfiMdssHandoffProtocolGuid,gEfiCrRamRangeProtocolGuid;
static ARM_MEMORY_REGION_DESCRIPTOR_EX ReservationMap[2];
static EFI_CR_RAM_RANGE_PROTOCOL RamRange;
static VOID *Backing;
static EFI_HANDLE BackingOwner;
EFI_GUID gEfiConsoleOutDeviceGuid,gEfiStandardErrorDeviceGuid;
EFI_GUID gEfiGraphicsOutputProtocolGuid,gEfiDevicePathProtocolGuid,gEfiSimpleTextOutProtocolGuid,gEfiEventExitBootServicesGuid;
static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static EFI_SYSTEM_TABLE SystemTable;
EFI_SYSTEM_TABLE *gST=&SystemTable;
static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS=&DxeServices;
static UINT64 GcdAttributes,GcdCapabilities;
static UINT64 GcdProtected;
static UINTN CacheTransitions,CacheCleans;
static BOOLEAN FailCacheSet,CommitCacheSet,FailCacheRestore,BadCacheReadback,GcdOverflow;
static EFI_USB_TYPEC_STATE State;
static EFI_USB_TYPEC_CR_PROTOCOL TypeC;
static BOOLEAN TypeCPublished;
static EFI_USB_DP_PHY_CR_PROTOCOL Phy;
static EFI_CLOCK_CR_PROTOCOL Clock;
static EFI_INTERCONNECT_CR_PROTOCOL Icb;
static EFI_SMMU_CR_PROTOCOL Smmu;
static EDKII_IOMMU_PROTOCOL Iommu;
static EFI_MDSS_HANDOFF_PROTOCOL Handoff;
static UINTN Registrations,Unregistrations;
static EFI_STATUS RegistrationStatus,UnregistrationStatus;
static EFI_MDSS_HANDOFF_FREEZE RegisteredFreeze;
static EFI_MDSS_HANDOFF_SNAPSHOT RegisteredSnapshot;
static UINTN ClockVotes, PathVotes, Starts, Stops, Maps, Unmaps, Preserves, Attributes, Irqs, Creates, FailCreate;
static BOOLEAN PhyVote, Aux, Scanout, FailStop, FailUnmap;
static BOOLEAN FailPhyAcquire, FailPhyRelease;
static UINTN PhyReleases,Allocations,Frees,CacheFlushes;
static UINTN LiveSnapshots;
static UINTN LiveLinks;
static BOOLEAN FailAllocate,FailFree;
static UINTN Installs,Connects,SourceQueries,TplDepth,FlushBytes;
static EFI_STATUS InstallStatus;
static EFI_STATUS ConnectStatus;
static BOOLEAN TextReady,DelayText;
static BOOLEAN MarkerReady,SplitterReady,DelayMarker,DelaySplitter,ConPlatformBound,WrongSplitter;
static UINTN ConsoleAppends[2],ConsoleVariables[2];
static EFI_STATUS VariableStatus,InfoStatus;
static CONSOLE_TYPE VariableFailureType;
static EFI_DRIVER_BINDING_PROTOCOL PlatformBinding={.DriverBindingHandle=(EFI_HANDLE)0xC1};
static EFI_DRIVER_BINDING_PROTOCOL SplitterBinding={.DriverBindingHandle=(EFI_HANDLE)0xC2};
static EFI_HANDLE SplitterHandle=(EFI_HANDLE)0xC0;
static EFI_STATUS EFIAPI ConPlatformTextOutDriverBindingStart(EFI_DRIVER_BINDING_PROTOCOL *,EFI_HANDLE,EFI_DEVICE_PATH_PROTOCOL *);
static EFI_STATUS ConSplitterStart(EFI_DRIVER_BINDING_PROTOCOL *,EFI_HANDLE,EFI_HANDLE,EFI_GUID *,EFI_GUID *,VOID **);
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL TextOutput;
static BOOLEAN CommitInstall;
#ifndef MDEPKG_NDEBUG
#define COLOR_BARS_BUILD TRUE
#else
#define COLOR_BARS_BUILD FALSE
#endif
static EFI_STATUS TimerStatus, AuxStatus,LiveLinkStatus;
static EFI_EVENT_NOTIFY TimerCallback, ExitCallback;
static BOOLEAN FreezeDuringQuery;

VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memmove(D,S,N); }
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V){return memset(D,V,N);}
VOID *EFIAPI SetMem32(VOID *D,UINTN N,UINT32 V){assert(!(N&3));for(UINTN I=0;I<N;I+=4)memcpy((UINT8 *)D+I,&V,4);return D;}
VOID *EFIAPI SetMem64(VOID *D,UINTN N,UINT64 V){assert(!(N&7));for(UINTN I=0;I<N;I+=8)memcpy((UINT8 *)D+I,&V,8);return D;}
INTN EFIAPI HighBitSet32(UINT32 V){if(!V)return -1;INTN I=0;while(V>>=1)++I;return I;}
VOID *EFIAPI AllocatePool(UINTN N){return malloc(N);}
VOID *EFIAPI AllocateCopyPool(UINTN N,CONST VOID *P){
  if(FreezeDuringQuery){FreezeDuringQuery=FALSE;UINTN Prior=Stops;
    assert(mGopBltBusy&&RegisteredFreeze&&RegisteredFreeze(&mRoute)==EFI_NOT_READY&&
      mExiting&&!mFrozen&&Stops==Prior&&!CrDpGopFrozen());}
  VOID *D=malloc(N);if(D)memcpy(D,P,N);return D;
}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){fprintf(stderr,"assert %s:%zu %s\n",F,(size_t)L,D);abort();}
VOID EFIAPI FreePool(VOID *P) { free(P); }
VOID EFIAPI MemoryFence(VOID) {}
BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
UINTN EFIAPI MicroSecondDelay(UINTN N) { return N; }
VOID *EFIAPI WriteBackDataCacheRange(VOID *P, UINTN N) {
  assert(mGopPages&&(UINTN)P>=mGopMode.FrameBufferBase&&(UINTN)P+N<=mGopMode.FrameBufferBase+mGopMode.FrameBufferSize&&!TplDepth);
  ++CacheFlushes;FlushBytes+=N;return P;
}
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *P,UINTN N){
  assert((UINTN)P==mGopMode.FrameBufferBase&&N==EFI_PAGES_TO_SIZE(mGopPages)&&!Scanout&&!mMapping&&!TplDepth);
  ++CacheCleans;return P;
}
static EFI_STATUS EFIAPI GcdGet(EFI_PHYSICAL_ADDRESS Address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  assert(Address==ReservationMap[0].Address);
  *D=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){.BaseAddress=Address,.Length=CR_DP_RESERVATION_BYTES,.ImageHandle=BackingOwner,
    .GcdMemoryType=EfiGcdMemoryTypeReserved,.Attributes=GcdAttributes,.Capabilities=GcdCapabilities};
  D->Attributes|=GcdProtected;
  if(GcdOverflow){D->BaseAddress=1;D->Length=MAX_UINT64;}
  if(BadCacheReadback&&CacheTransitions)D->Attributes=EFI_MEMORY_WB|EFI_MEMORY_XP;
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GcdSet(EFI_PHYSICAL_ADDRESS Address,UINT64 Length,UINT64 AttributesValue){
  assert(Address==mGopMode.FrameBufferBase&&Length==EFI_PAGES_TO_SIZE(mGopPages)&&!Scanout&&!mMapping&&!TplDepth);
  assert(AttributesValue==(EFI_MEMORY_WB|EFI_MEMORY_XP)||AttributesValue==(EFI_MEMORY_WC|EFI_MEMORY_XP));
  ++CacheTransitions;
  if(AttributesValue==(EFI_MEMORY_WC|EFI_MEMORY_XP)&&FailCacheSet){if(CommitCacheSet)GcdAttributes=AttributesValue;return EFI_DEVICE_ERROR;}
  if(AttributesValue==(EFI_MEMORY_WB|EFI_MEMORY_XP)&&FailCacheRestore)return EFI_DEVICE_ERROR;
  GcdAttributes=AttributesValue;return EFI_SUCCESS;
}

UINT32 EFIAPI MmioRead32(UINTN A) { assert(A==0xAF081B4 || A==0xAE90400); return 0; }
UINT32 EFIAPI MmioWrite32(UINTN A, UINT32 V) { assert(A==0xAE90010 || A==0xAE90400 || A==0xAE90404); return V; }
UINT32 EFIAPI MmioOr32(UINTN A, UINT32 V) { return MmioWrite32(A,V); }
UINT32 EFIAPI MmioAnd32(UINTN A, UINT32 V) { return MmioWrite32(A,V); }
VOID CrDpAuxEnable(VOID) { assert(PhyVote); Aux=TRUE; }
VOID CrDpAuxDisable(VOID) { assert(Aux && !Scanout); Aux=FALSE; }
EFI_STATUS CrDpAuxTransfer(BOOLEAN Native, BOOLEAN Read, UINT32 A, UINT8 *P, UINTN N)
{
  assert(Native && Aux);
  if(EFI_ERROR(AuxStatus)) return AuxStatus;
  if(Read) { assert(A==0 && N==16); memset(P,0,N); P[0]=0x12; P[1]=0x14; P[2]=0x84; }
  else assert(A==0x600 && N==1 && P[0]==1);
  return EFI_SUCCESS;
}
EFI_STATUS CrDpReadEdid(UINT8 B, UINT8 *P) { assert(B==0 && Aux); memset(P,0,128); return EFI_SUCCESS; }
EFI_STATUS CrDpFindMirrorTiming(CONST UINT8 *P, UINTN N, UINT32 W, UINT32 H, CR_DP_TIMING *T)
{ assert(P && N==128 && W==1920 && H==1080); *T=(CR_DP_TIMING){.PixelHz=148500000,.Width=1920,.Height=1080,
  .HFront=88,.HSync=44,.HBack=148,.VFront=4,.VSync=5,.VBack=36}; return EFI_SUCCESS; }
EFI_STATUS CrDpCalculateTu(CONST CR_DP_TIMING *T, UINT32 R, UINT8 L, CR_DP_TU *Tu)
{ assert(T->PixelHz==148500000); *Tu=(CR_DP_TU){40,33,15}; return R==2700 && L==2 ? EFI_SUCCESS : EFI_UNSUPPORTED; }
EFI_STATUS CrDpTrain(EFI_USB_DP_PHY_CR_PROTOCOL *P, CONST UINT8 *D, UINT32 R, UINT8 L)
{ assert(P==&Phy && D[0]==0x12 && R==2700 && L==2 && Aux); return EFI_SUCCESS; }
VOID CrDpConfigureStream(CONST CR_DP_TIMING *T, UINT32 R, CONST CR_DP_TU *Tu)
{ assert(T->Width==1920 && R==2700 && Tu->Valid==33 && (Preserves==0&&CacheFlushes&&mBltConfig)); }
VOID CrDpStartScanout(CONST CR_DP_TIMING *T, EFI_PHYSICAL_ADDRESS Iova, UINT32 Stride)
{ assert(T->Width==1920 && Iova==0x10000000 && Stride==7680 && Attributes%4==0); Scanout=TRUE; ++Starts; }
EFI_STATUS CrDpWait(UINTN A, UINT32 M, UINT32 E, UINT32 Us)
{ assert(A==0xAE90440 && M==1 && E==1 && Us==100000); return EFI_SUCCESS; }
EFI_STATUS CrDpWaitScanout(VOID) { assert(Scanout); return EFI_SUCCESS; }
VOID CrDpSnapshotScanout(VOID) { assert(Scanout && ClockVotes==12 && mGopPages); ++LiveSnapshots; }
EFI_STATUS CrDpObserveLink(VOID) {
  assert(Scanout && ClockVotes==12 && mGopPages);
  ++LiveLinks;return LiveLinkStatus;
}
EFI_STATUS CrDpStopScanout(VOID) { ++Stops; if(FailStop) return EFI_TIMEOUT; Scanout=FALSE; return EFI_SUCCESS; }
EFI_STATUS CrDpServiceIrq(VOID) { ++Irqs; return EFI_SUCCESS; }

static EFI_STATUS EFIAPI GetState(EFI_USB_TYPEC_CR_PROTOCOL *P, EFI_USB_TYPEC_STATE *S)
{ assert(P==&TypeC && TypeCPublished); *S=State; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Acquire(EFI_USB_DP_PHY_CR_PROTOCOL *P, EFI_USB_DP_PHY_CLIENT C, CONST EFI_USB_TYPEC_STATE *S, BOOLEAN Ss, BOOLEAN *Owned)
{ assert(P==&Phy && C==UsbDpPhyDp && S->DpActive && !Ss && !PhyVote); PhyVote=*Owned=TRUE; return FailPhyAcquire ? EFI_DEVICE_ERROR : EFI_SUCCESS; }
static EFI_STATUS EFIAPI Release(EFI_USB_DP_PHY_CR_PROTOCOL *P, EFI_USB_DP_PHY_CLIENT C)
{ assert(P==&Phy && C==UsbDpPhyDp && PhyVote && !Scanout && !Aux && ClockVotes==0); ++PhyReleases; if(FailPhyRelease) return EFI_DEVICE_ERROR; PhyVote=FALSE; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Power(EFI_USB_DP_PHY_CR_PROTOCOL *P, UINT32 R, UINT8 L)
{ assert(P==&Phy && R==2700 && L==2 && ClockVotes==9); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Levels(EFI_USB_DP_PHY_CR_PROTOCOL *P, UINT8 V, UINT8 Pre)
{ assert(P==&Phy && V+Pre<=3); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI SetClock(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN E)
{ assert(P==&Clock && C && N); if(E) ++ClockVotes; else { assert(ClockVotes && !Scanout && R==0); --ClockVotes; } return EFI_SUCCESS; }
static EFI_STATUS EFIAPI AcquireClockMock(EFI_CLOCK_CR_PROTOCOL *P,
  CONST CHAR8 *C, CONST CHAR8 *N, UINT64 R, BOOLEAN *Owned)
{
  EFI_STATUS Status = SetClock(P, C, N, R, TRUE);
  *Owned = !EFI_ERROR(Status);
  return Status;
}
static EFI_STATUS EFIAPI External(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, UINT32 R, UINT32 S, UINT32 D, UINT32 M, UINT32 Den, UINT32 TwiceD)
{ assert(P==&Clock && strcmp(C,"dispcc")==0 && N && D==2); if(S==1) assert(R==270000000 && M==0 && Den==0 && TwiceD==0); else assert(S==2 && R==148500000 && M==11 && Den==100 && TwiceD==100); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Divider(EFI_CLOCK_CR_PROTOCOL *P, CONST CHAR8 *C, CONST CHAR8 *N, UINT32 D)
{ assert(P==&Clock && C && N && D==1); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Path(EFI_INTERCONNECT_CR_PROTOCOL *P, CONST CHAR8 *C, UINT32 S, UINT32 D, INTERCONNECT_PATH_HANDLE *H)
{ assert(P==&Icb && strcmp(C,"qcom,sm8450-mdp")==0 && D==S+1); *H=++PathVotes; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *P, INTERCONNECT_PATH_HANDLE H, UINT64 A, UINT64 B)
{ assert(P==&Icb && H && B && (A==0 || A==600000)); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI DropPath(EFI_INTERCONNECT_CR_PROTOCOL *P, INTERCONNECT_PATH_HANDLE H)
{ assert(P==&Icb && H && PathVotes && !PhyVote); --PathVotes; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Attach(EFI_SMMU_CR_PROTOCOL *P, UINT16 Seg, UINT16 Bdf, UINT16 Sid)
{ assert(P==&Smmu && Seg==0 && Bdf==0xFFFF && (Sid==0x2800 || Sid==0x2802 || Sid==0x2C00 || Sid==0x2C02)); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Preserve(EFI_SMMU_CR_PROTOCOL *P, VOID *T)
{ assert(P==&Smmu && T==(VOID *)0xA && Attributes%4==0); ++Preserves; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Map(EDKII_IOMMU_PROTOCOL *P, EDKII_IOMMU_OPERATION O, VOID *F, UINTN *N, EFI_PHYSICAL_ADDRESS *D, VOID **T)
{ assert(P==&Iommu&&O==EdkiiIoMmuOperationBusMasterRead64&&(UINTN)F==mGopMode.FrameBufferBase&&mGopPages&&*N==7680*1080&&mGopCacheReady&&GcdAttributes==(EFI_MEMORY_WC|EFI_MEMORY_XP));
 *D=0x10000000;*T=(VOID *)0xA;++Maps;return EFI_SUCCESS;}

static EFI_STATUS EFIAPI Unmap(EDKII_IOMMU_PROTOCOL *P, VOID *T)
{ assert(P==&Iommu && T==(VOID *)0xA && !Scanout); if(FailUnmap) return EFI_DEVICE_ERROR; ++Unmaps; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Attribute(EDKII_IOMMU_PROTOCOL *P, UINTN Base, UINT32 Sid, VOID *T, UINT64 A)
{ assert(P==&Iommu && Base==0x15000000 && Sid && T==(VOID *)0xA && A==1); ++Attributes; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ReserveTest(EFI_GCD_ALLOCATE_TYPE Type,EFI_GCD_MEMORY_TYPE Mem,UINTN Alignment,
  UINT64 Bytes,EFI_PHYSICAL_ADDRESS *Address,EFI_HANDLE Image,EFI_HANDLE Device)
{ assert(Type==EfiGcdAllocateAddress&&Mem==EfiGcdMemoryTypeReserved&&Alignment==16&&Bytes==CR_DP_RESERVATION_BYTES&&
    *Address==ReservationMap[0].Address&&Image==(EFI_HANDLE)1&&!Device&&!BackingOwner);
  if(FailAllocate)return EFI_OUT_OF_RESOURCES;
  BackingOwner=Image;++Allocations;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI ReleaseTest(EFI_PHYSICAL_ADDRESS Address,UINT64 Bytes)
{ assert(Address==ReservationMap[0].Address&&Bytes==CR_DP_RESERVATION_BYTES&&!Scanout&&mMapping==NULL&&BackingOwner==(EFI_HANDLE)1);
  assert(GcdAttributes==(EFI_MEMORY_WB|EFI_MEMORY_XP));
  if(FailFree)return EFI_DEVICE_ERROR;BackingOwner=NULL;++Frees;return EFI_SUCCESS; }
static EFI_STATUS EFIAPI RangeTest(EFI_CR_RAM_RANGE_PROTOCOL *This,UINT64 Base,UINT64 Bytes){
  assert(This==&RamRange&&Base==ReservationMap[0].Address&&Bytes==CR_DP_RESERVATION_BYTES);return EFI_SUCCESS;
}
ARM_MEMORY_REGION_DESCRIPTOR_EX *GetPlatformMemoryMap(VOID){return ReservationMap;}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
static EFI_STATUS EFIAPI Locate(EFI_GUID *G, VOID *R, VOID **P)
{ assert(R==NULL); if(G==&gEfiUsbTypeCCrProtocolGuid && !TypeCPublished) return EFI_NOT_FOUND; *P=G==&gEfiUsbTypeCCrProtocolGuid ? (VOID *)&TypeC : G==&gEfiUsbDpPhyCrProtocolGuid ? (VOID *)&Phy : G==&gEfiClockCrProtocolGuid ? (VOID *)&Clock : G==&gEfiInterconnectCrProtocolGuid ? (VOID *)&Icb : G==&gEfiSmmuCrProtocolGuid ? (VOID *)&Smmu : G==&gEfiMdssHandoffProtocolGuid?(VOID *)&Handoff:G==&gEfiCrRamRangeProtocolGuid?(VOID *)&RamRange:(VOID *)&Iommu; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Participant(EFI_MDSS_HANDOFF_PROTOCOL *This,UINT32 Owner,VOID *Context,
  EFI_MDSS_HANDOFF_FREEZE Freeze,EFI_MDSS_HANDOFF_SNAPSHOT Snapshot){
  assert(This==&Handoff&&Owner==CR_MDSS_NATIVE_DP&&Context==&mRoute);
  if(Freeze){assert(Snapshot&&!Creates&&!ClockVotes&&!PathVotes&&!Allocations);
    ++Registrations;if(RegistrationStatus!=EFI_SUCCESS)return RegistrationStatus;
    RegisteredFreeze=Freeze;RegisteredSnapshot=Snapshot;}
  else{assert(!Snapshot&&!mGopPages&&!Scanout&&!ClockVotes&&!PathVotes&&!mMapping);
    ++Unregistrations;if(UnregistrationStatus!=EFI_SUCCESS)return UnregistrationStatus;RegisteredFreeze=NULL;RegisteredSnapshot=NULL;}
  return EFI_SUCCESS;
}
static EFI_TPL EFIAPI Raise(EFI_TPL T){assert(T==TPL_NOTIFY&&!TplDepth);++TplDepth;return TPL_APPLICATION;}
static VOID EFIAPI Restore(EFI_TPL T){assert(T==TPL_APPLICATION&&TplDepth==1);--TplDepth;}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *H,...){
  va_list A;va_start(A,H);EFI_GUID *G=va_arg(A,EFI_GUID *);VOID *P=va_arg(A,VOID *);va_end(A);
  assert(mActive&&mBltConfig&&!TplDepth);
  if(G==&gEfiConsoleOutDeviceGuid||G==&gEfiStandardErrorDeviceGuid){
    assert(*H==(EFI_HANDLE)0xD9&&P==NULL);if(G==&gEfiConsoleOutDeviceGuid)MarkerReady=TRUE;return EFI_SUCCESS;}
  assert(G==&gEfiDevicePathProtocolGuid&&P==&mGopPath);++Installs;
  if(InstallStatus==EFI_SUCCESS||CommitInstall)*H=(EFI_HANDLE)0xD9;return InstallStatus;
}
static EFI_STATUS EFIAPI Connect(EFI_HANDLE H,EFI_HANDLE *Drivers,EFI_DEVICE_PATH_PROTOCOL *Path,BOOLEAN Recursive){
  assert(H==(EFI_HANDLE)0xD9&&!Drivers&&!Path&&Recursive&&!mBusy&&!TplDepth&&mActive);++Connects;
  if(ConnectStatus==EFI_SUCCESS&&!DelayText){TextReady=TRUE;
    if(!DelayMarker&&!ConPlatformBound)assert(ConPlatformTextOutDriverBindingStart(&PlatformBinding,H,NULL)==EFI_SUCCESS);
    if(MarkerReady&&!DelaySplitter&&!SplitterReady){VOID *Text=NULL;
      assert(ConSplitterStart(&SplitterBinding,H,SplitterHandle,&gEfiConsoleOutDeviceGuid,
        &gEfiSimpleTextOutProtocolGuid,&Text)==EFI_SUCCESS&&Text==&TextOutput);}}
  return ConnectStatus;
}
static EFI_STATUS EFIAPI TextString(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,CHAR16 *String){(VOID)This;(VOID)String;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Handle(EFI_HANDLE H,EFI_GUID *G,VOID **P){
  if(H==SplitterHandle&&G==&gEfiSimpleTextOutProtocolGuid){*P=&TextOutput;return EFI_SUCCESS;}
  if(H==(EFI_HANDLE)0xBAD)return EFI_NOT_FOUND;
  assert(H==(EFI_HANDLE)0xD9&&mGopPublished&&!TplDepth);
  if(G==&gEfiConsoleOutDeviceGuid){*P=NULL;return MarkerReady?EFI_SUCCESS:EFI_NOT_FOUND;}
  assert(G==&gEfiSimpleTextOutProtocolGuid);if(!TextReady)return EFI_NOT_FOUND;*P=&TextOutput;return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Open(EFI_HANDLE H,EFI_GUID *G,VOID **P,EFI_HANDLE Agent,EFI_HANDLE Controller,UINT32 AttributesValue){
  assert(H==(EFI_HANDLE)0xD9&&!TplDepth);
  if(G==&gEfiDevicePathProtocolGuid){assert(AttributesValue==EFI_OPEN_PROTOCOL_GET_PROTOCOL);*P=&mGopPath;return EFI_SUCCESS;}
  if(G==&gEfiSimpleTextOutProtocolGuid){if(!TextReady)return EFI_NOT_FOUND;*P=&TextOutput;
    if(AttributesValue==EFI_OPEN_PROTOCOL_BY_DRIVER){assert(Agent==PlatformBinding.DriverBindingHandle&&Controller==H);ConPlatformBound=TRUE;}
    return EFI_SUCCESS;}
  assert(G==&gEfiConsoleOutDeviceGuid&&Agent==SplitterBinding.DriverBindingHandle);*P=NULL;
  if(!MarkerReady)return EFI_NOT_FOUND;
  if(AttributesValue==EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER){assert(Controller==SplitterHandle);SplitterReady=TRUE;}
  else assert(AttributesValue==EFI_OPEN_PROTOCOL_BY_DRIVER&&Controller==H);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI CloseProtocol(EFI_HANDLE H,EFI_GUID *G,EFI_HANDLE Agent,EFI_HANDLE Controller){
  assert(H==(EFI_HANDLE)0xD9&&!TplDepth);
  if(G==&gEfiSimpleTextOutProtocolGuid){assert(Agent==PlatformBinding.DriverBindingHandle&&Controller==H);ConPlatformBound=FALSE;}
  else assert(G==&gEfiConsoleOutDeviceGuid&&Agent==SplitterBinding.DriverBindingHandle);
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI OpenInfo(EFI_HANDLE H,EFI_GUID *G,EFI_OPEN_PROTOCOL_INFORMATION_ENTRY **E,UINTN *N){
  assert(H==(EFI_HANDLE)0xD9&&G==&gEfiConsoleOutDeviceGuid&&!TplDepth);*E=NULL;*N=0;
  if(InfoStatus!=EFI_SUCCESS)return InfoStatus;
  if(SplitterReady){EFI_OPEN_PROTOCOL_INFORMATION_ENTRY Entries[2]={
    {SplitterBinding.DriverBindingHandle,H,EFI_OPEN_PROTOCOL_BY_DRIVER,1},
    {SplitterBinding.DriverBindingHandle,WrongSplitter?(EFI_HANDLE)0xBAD:SplitterHandle,EFI_OPEN_PROTOCOL_BY_CHILD_CONTROLLER,1}};
    *E=AllocateCopyPool(sizeof(Entries),Entries);assert(*E);*N=2;}
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI EfiBootManagerUpdateConsoleVariable(CONSOLE_TYPE Type,EFI_DEVICE_PATH_PROTOCOL *Add,EFI_DEVICE_PATH_PROTOCOL *Remove){
  assert((Type==ConOut||Type==ErrOut)&&Add==(VOID *)&mGopPath&&!Remove&&!TplDepth&&mActive);
  ++ConsoleAppends[Type==ErrOut];if(VariableStatus!=EFI_SUCCESS&&Type==VariableFailureType)return VariableStatus;
  ConsoleVariables[Type==ErrOut]|=2;return EFI_SUCCESS;
}
typedef enum {Check,Append,Delete} CONPLATFORM_VAR_OPERATION;
static EFI_STATUS ConPlatformUpdateDeviceVariable(CHAR16 *Name,EFI_DEVICE_PATH_PROTOCOL *Path,CONPLATFORM_VAR_OPERATION Op){
  assert(Path==(VOID *)&mGopPath&&(Op==Check||Op==Append));
  if(Op==Append)return EFI_SUCCESS;
  assert(Name[0]=='C'||Name[0]=='E');return ConsoleVariables[Name[0]=='E']&2?EFI_SUCCESS:EFI_NOT_FOUND;
}
static BOOLEAN ConPlatformUpdateGopCandidate(EFI_DEVICE_PATH_PROTOCOL *Path){assert(Path==(VOID *)&mGopPath);return FALSE;}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-parameter"
#include "dp_console_bindings.inc"
#pragma clang diagnostic pop
static EFI_STATUS EFIAPI CreateTimer(UINT32 T, EFI_TPL L, EFI_EVENT_NOTIFY N, VOID *C, EFI_EVENT *E)
{ assert(T==(EVT_TIMER|EVT_NOTIFY_SIGNAL) && L==TPL_CALLBACK && C==NULL); if(++Creates==FailCreate) return EFI_OUT_OF_RESOURCES; TimerCallback=N; *E=(EFI_EVENT)1; N(*E,C);assert(!Maps); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI CreateExit(UINT32 T, EFI_TPL L, EFI_EVENT_NOTIFY N, CONST VOID *C, CONST EFI_GUID *G, EFI_EVENT *E)
{ assert(T==EVT_NOTIFY_SIGNAL && L==TPL_CALLBACK && C==NULL && G==&gEfiEventExitBootServicesGuid); if(++Creates==FailCreate) return EFI_OUT_OF_RESOURCES; ExitCallback=N; *E=(EFI_EVENT)2; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Timer(EFI_EVENT E, EFI_TIMER_DELAY D, UINT64 N)
{ assert(E==(EFI_EVENT)1 && (D==TimerCancel ? N==0 : D==TimerPeriodic && N==1000000)); return D==TimerCancel ? EFI_SUCCESS : TimerStatus; }
static EFI_STATUS EFIAPI Close(EFI_EVENT E) { assert(E==(EFI_EVENT)1 || E==(EFI_EVENT)2); return EFI_SUCCESS; }
static VOID Fixture(VOID)
{
  assert(!ClockVotes&&!PathVotes&&!PhyVote&&Maps==Unmaps&&Allocations==Frees&&mGopPages==0);
  mTimer=mExit=NULL;mBusy=mExiting=mConnecting=mConnectPending=mConsoleConnected=FALSE;
  mConnectAttempts=mConnectRetryTicks=mRetryTicks=mCleanupRetryTicks=0;mHaveRetryRoute=FALSE;
  Creates=FailCreate=Starts=Stops=Maps=Unmaps=Preserves=Attributes=Irqs=0;
  FailStop=FailUnmap=FALSE;TimerStatus=AuxStatus=EFI_SUCCESS;
  FailPhyAcquire=FailPhyRelease=FALSE;PhyReleases=0;FailAllocate=FailFree=FALSE;Allocations=Frees=CacheFlushes=FlushBytes=0;
  LiveSnapshots=LiveLinks=0;LiveLinkStatus=EFI_SUCCESS;
  Installs=Connects=SourceQueries=0;InstallStatus=ConnectStatus=EFI_SUCCESS;CommitInstall=FALSE;
  TextReady=DelayText=FALSE;TextOutput=(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL){.OutputString=TextString};
  ConsolePolicy=TRUE;MarkerReady=SplitterReady=DelayMarker=DelaySplitter=ConPlatformBound=WrongSplitter=FALSE;
  ConsoleVariables[0]=ConsoleVariables[1]=1;ConsoleAppends[0]=ConsoleAppends[1]=0;
  VariableStatus=InfoStatus=EFI_SUCCESS;
  VariableFailureType=ConOut;
  SystemTable=(EFI_SYSTEM_TABLE){.ConsoleOutHandle=(EFI_HANDLE)0xC0,.ConOut=&TextOutput};
  if(!Backing){Backing=aligned_alloc(65536,CR_DP_RESERVATION_BYTES);assert(Backing);}
  memset(Backing,0xa5,CR_DP_RESERVATION_BYTES);BackingOwner=NULL;
  ReservationMap[0]=(ARM_MEMORY_REGION_DESCRIPTOR_EX){.Name=CR_DP_RESERVATION_NAME,.Address=(UINTN)Backing,
    .Length=CR_DP_RESERVATION_BYTES,.HobOption=AddMem,.ResourceType=MEM_RES,.ResourceAttribute=SYS_MEM_CAP,
    .MemoryType=Reserv,.ArmAttributes=UNCACHED_UNBUFFERED_XN};
  ReservationMap[1]=(ARM_MEMORY_REGION_DESCRIPTOR_EX){0};
  RamRange=(EFI_CR_RAM_RANGE_PROTOCOL){EFI_CR_RAM_RANGE_PROTOCOL_REVISION,RangeTest};
  mReservationImage=NULL;mReservationRam=NULL;mReservationBase=0;
  mReservationClaimAttempted=mReservationFreeAttempted=mReservationUnknownAddress=FALSE;
  GcdAttributes=EFI_MEMORY_WB|EFI_MEMORY_XP;GcdCapabilities=CR_DP_RESERVED_STATE_CAPS|EFI_MEMORY_WB|EFI_MEMORY_WC|EFI_MEMORY_XP;
  GcdProtected=0;CacheTransitions=CacheCleans=0;FailCacheSet=CommitCacheSet=FailCacheRestore=BadCacheReadback=GcdOverflow=FALSE;
  DxeServices=(EFI_DXE_SERVICES){.GetMemorySpaceDescriptor=GcdGet,.SetMemorySpaceAttributes=GcdSet,.AllocateMemorySpace=ReserveTest,.FreeMemorySpace=ReleaseTest};
  mGopHandle=NULL;mGopPublished=mGopPublicationPending=mGopExiting=mGopBltBusy=FALSE;
  mGopConOutAdded=mGopErrOutAdded=FALSE;
  mHandoffRegistered=mFrozen=mHaveTiming=FALSE;mFreezeStatus=mRetainedError=EFI_SUCCESS;
  mMappedIova=mMappedBytes=mSourceRate=mInterfaceDivider=mPixelM=mPixelN=0;ZeroMem(&mTiming,sizeof(mTiming));
  Registrations=Unregistrations=0;RegistrationStatus=UnregistrationStatus=EFI_SUCCESS;RegisteredFreeze=NULL;RegisteredSnapshot=NULL;
  FreezeDuringQuery=FALSE;
  Handoff=(EFI_MDSS_HANDOFF_PROTOCOL){EFI_MDSS_HANDOFF_PROTOCOL_REVISION,Participant};
  State=(EFI_USB_TYPEC_STATE){.PdReady=TRUE,.Connected=TRUE,.DpActive=TRUE,.Hpd=TRUE,.DpLanes=4,.Sequence=1};
  TypeCPublished=TRUE;TypeC.Revision=EFI_USB_TYPEC_CR_PROTOCOL_REVISION;
  TypeC.GetState=GetState;Phy.Acquire=Acquire;Phy.Release=Release;Phy.PowerDp=Power;Phy.SetDpLevels=Levels;
  Clock.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION;Clock.AcquireClock=AcquireClockMock;
  Clock.SetClock=SetClock;Clock.ConfigureExternalSource=External;Clock.SetDivider=Divider;
  Icb.AcquirePath=Path;Icb.SetBandwidth=Bandwidth;Icb.ReleasePath=DropPath;
  Smmu.Revision=EFI_SMMU_CR_PRESERVE_MAPPING_REVISION;Smmu.Attach=Attach;Smmu.PreserveReadOnlyMapping=Preserve;
  Iommu.Revision=EDKII_IOMMU_PROTOCOL_REVISION;Iommu.Map=Map;Iommu.Unmap=Unmap;Iommu.SetAttributeById=Attribute;
  Services=(EFI_BOOT_SERVICES){.LocateProtocol=Locate,
    .CreateEvent=CreateTimer,.CreateEventEx=CreateExit,.SetTimer=Timer,.CloseEvent=Close,
    .RaiseTPL=Raise,.RestoreTPL=Restore,.InstallMultipleProtocolInterfaces=Install,.ConnectController=Connect,.HandleProtocol=Handle,
    .OpenProtocol=Open,.CloseProtocol=CloseProtocol,.OpenProtocolInformation=OpenInfo};
}
/* Host fixture simulates reboot after proving retained clients/modes; native
 * production deliberately never frees published GOP storage during hotplug. */
static VOID RebootFixture(VOID){
  mExiting=FALSE;mGopExiting=FALSE;FailStop=FailUnmap=FailFree=FailCacheRestore=FALSE;BadCacheReadback=FALSE;
  assert(Stop()==EFI_SUCCESS);mGopPublished=mGopPublicationPending=FALSE;mGopHandle=NULL;
  assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS&&Allocations==Frees);
}
static VOID RetryCleanup(VOID)
{
  while(mCleanupRetryTicks) TimerCallback((EFI_EVENT)1,NULL);
  TimerCallback((EFI_EVENT)1,NULL);
}
static VOID GopPixels(VOID)
{
  UINTN Size=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=NULL;
  assert(mGop.QueryMode(&mGop,1,&Size,&Info)==EFI_UNSUPPORTED);
  assert(mGop.QueryMode(&mGop,0,&Size,&Info)==EFI_SUCCESS&&Size==sizeof(*Info)&&Info!=&mGopInfo&&
    Info->HorizontalResolution==1920&&Info->VerticalResolution==1080&&Info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor);
  free(Info);assert(mGop.SetMode(&mGop,1)==EFI_UNSUPPORTED);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Red={.Blue=0x31,.Green=0x52,.Red=0x73,.Reserved=0};
  UINTN Flush=FlushBytes,Count=CacheFlushes;
  assert(mGop.Blt(&mGop,&Red,EfiBltVideoFill,0,0,2,3,2,3,0)==EFI_SUCCESS);
  assert(FlushBytes==Flush+24&&CacheFlushes==Count+3);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Out[12];memset(Out,0xa5,sizeof(Out));
  assert(mGop.Blt(&mGop,Out,EfiBltVideoToBltBuffer,2,3,1,0,2,3,12)==EFI_SUCCESS);
  assert(memcmp(&Out[1],&Red,4)==0&&memcmp(&Out[4],&Red,4)==0&&CacheFlushes==Count+3);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Saved=Out[0];assert(Saved.Blue==0xa5); /* caller padding preserved */
  assert(mGop.Blt(&mGop,&Red,EfiBltVideoFill,0,0,MAX_UINTN,0,1,1,0)==EFI_INVALID_PARAMETER);
  assert(mGop.Blt(&mGop,Out,EfiBltBufferToVideo,MAX_UINTN,0,0,0,1,1,4)==EFI_INVALID_PARAMETER);
  assert(mGop.Blt(&mGop,Out,EfiBltVideoToBltBuffer,0,0,0,MAX_UINTN,1,2,4)==EFI_INVALID_PARAMETER);
  assert(mGop.Blt(&mGop,NULL,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_INVALID_PARAMETER);
  assert(mGop.Blt(&mGop,NULL,EfiBltVideoToVideo,2,3,3,4,2,3,0)==EFI_SUCCESS);
  assert(mGop.Blt(&mGop,Out,EfiBltVideoToBltBuffer,3,4,0,0,2,1,0)==EFI_SUCCESS&&memcmp(Out,&Red,4)==0);
}
static VOID SnapshotAccepted(CONST CR_MDSS_HANDOFF_PARTICIPANT *P){
  CR_MDSS_NATIVE_HANDOFF Record={0};
  assert(CrMdssHandoffInit(&Record,1,2,0xF0000000,8192,CR_MDSS_NATIVE_ALL_OWNERS,CR_MDSS_NATIVE_ALL_OWNERS)==CR_SUCCESS);
  assert(CrMdssHandoffAppend(&Record,P)==CR_SUCCESS&&Record.FrozenOwners==CR_MDSS_NATIVE_DP);
}
static VOID HandoffLifecycle(VOID){
  CR_MDSS_HANDOFF_PARTICIPANT P;VOID *Token=NULL;
  Fixture();RegistrationStatus=EFI_DEVICE_ERROR;
  assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_DEVICE_ERROR&&!Creates&&!Maps&&!mHandoffRegistered);
  Fixture();State.Hpd=FALSE;assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  assert(Registrations==1&&RegisteredFreeze&&RegisteredSnapshot);TimerCallback((EFI_EVENT)1,NULL);
  assert(RegisteredSnapshot(&mRoute,&P,&Token)==EFI_NOT_READY);
  assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS);
  assert(RegisteredSnapshot(&mRoute,&P,&Token)==EFI_SUCCESS&&!Token&&P.Route.State==CR_MDSS_NATIVE_ABSENT&&!P.ClockCount&&!P.PathCount);
  SnapshotAccepted(&P);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  FreezeDuringQuery=TRUE;UINTN Size=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=NULL;
  assert(mGop.QueryMode(&mGop,0,&Size,&Info)==EFI_SUCCESS&&Info&&mExiting&&!mFrozen&&!mGopBltBusy);
  FreePool(Info);assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&mFrozen&&
    RegisteredSnapshot(&mRoute,&P,&Token)==EFI_SUCCESS);SnapshotAccepted(&P);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  UINTN Prior=Stops,Allocated=Allocations,Freed=Frees,Mapped=Maps,Unmapped=Unmaps,Installed=Installs;
  mBusy=TRUE;assert(RegisteredFreeze(&mRoute)==EFI_NOT_READY&&!mFrozen&&mExiting&&Stops==Prior);
  mBusy=FALSE;mConnecting=TRUE;assert(RegisteredFreeze(&mRoute)==EFI_NOT_READY&&!mFrozen&&Stops==Prior);
  mConnecting=FALSE;mGopBltBusy=TRUE;assert(RegisteredFreeze(&mRoute)==EFI_NOT_READY&&!mFrozen&&Stops==Prior);
  mGopBltBusy=FALSE;EFI_TPL PriorTpl=gBS->RaiseTPL(TPL_NOTIFY);
  assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&mFrozen&&!mScanout&&Stops==Prior+1);
  assert(RegisteredSnapshot(&mRoute,&P,&Token)==EFI_SUCCESS&&Token==(VOID *)0xA&&P.Route.State==CR_MDSS_NATIVE_STOPPED&&
    P.Route.Flags==CR_MDSS_NATIVE_OWNED&&P.ClockCount==12&&P.PathCount==2&&!P.RailCount&&!P.DomainCount&&
    P.Route.Mode.Width==1920&&P.Route.Mode.Height==1080&&P.Route.Mode.HSync==44&&P.Route.Mode.VSync==5&&
    P.Route.FrameBuffer==mGopMode.FrameBufferBase&&P.Route.FrameBufferBytes==7680*1080&&P.Route.DeviceAddress==0x10000000&&
    !P.Route.MappingOwner&&!P.Route.MappingId&&!P.Route.StreamCount&&P.Route.SourceRate==270000000&&P.Route.SourceDivider==2&&
    P.Route.InterfaceDivider==1&&P.Route.PixelM==11&&P.Route.PixelN==100&&
    P.Paths[0].Average==600000&&P.Paths[0].Peak==2500000&&P.Paths[1].Average==0&&P.Paths[1].Peak==19200);
  gBS->RestoreTPL(PriorTpl);
  assert(Allocations==Allocated&&Frees==Freed&&Maps==Mapped&&Unmaps==Unmapped&&Installs==Installed&&ClockVotes==12&&PathVotes==2&&PhyVote);
  SnapshotAccepted(&P);assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&Stops==Prior+1);
  ExitCallback((EFI_EVENT)2,NULL);assert(Stops==Prior+1);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  FailStop=TRUE;Prior=Stops;assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&mFrozen&&mFreezeStatus==EFI_TIMEOUT&&mScanout);
  assert(RegisteredSnapshot(&mRoute,&P,&Token)==EFI_SUCCESS&&P.Route.State==CR_MDSS_NATIVE_UNCERTAIN&&
    (P.Route.Flags&CR_MDSS_NATIVE_RETAINED_ERROR)&&Token==(VOID *)0xA&&P.ClockCount==12&&P.PathCount==2);
  SnapshotAccepted(&P);FailStop=FALSE;assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&Stops==Prior+1&&mScanout&&mFreezeStatus==EFI_TIMEOUT);
  ExitCallback((EFI_EVENT)2,NULL);assert(Stops==Prior+1);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  State.Hpd=FALSE;TimerCallback((EFI_EVENT)1,NULL);assert(mGopPages&&!mMapping&&!ClockVotes&&!PathVotes);
  assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&RegisteredSnapshot(&mRoute,&P,&Token)==EFI_SUCCESS&&
    !Token&&P.Route.State==CR_MDSS_NATIVE_STOPPED&&!P.Route.DeviceAddress&&P.Route.FrameBufferBytes==7680*1080&&
    P.Route.Mode.Width==1920&&!P.ClockCount&&!P.PathCount);SnapshotAccepted(&P);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);FailPhyAcquire=FailPhyRelease=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(mPhyOwned&&!mGopPages);
  assert(RegisteredFreeze(&mRoute)==EFI_SUCCESS&&RegisteredSnapshot(&mRoute,&P,&Token)==EFI_SUCCESS&&
    P.Route.State==CR_MDSS_NATIVE_UNCERTAIN&&(P.Route.Flags&CR_MDSS_NATIVE_RETAINED_ERROR)&&!P.Route.Mode.Width&&!Token);
  SnapshotAccepted(&P);FailPhyAcquire=FailPhyRelease=FALSE;RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);UnregistrationStatus=EFI_ACCESS_DENIED;
  assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED&&mHandoffRegistered&&mTimer&&mExit);
  UnregistrationStatus=EFI_SUCCESS;assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS&&!mHandoffRegistered&&!RegisteredFreeze&&Unregistrations==2);
}
int main(void)
{
  for(UINTN F=1;F<=2;++F){Fixture();FailCreate=F;assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_OUT_OF_RESOURCES&&!mTimer&&!mExit&&
    Registrations==1&&Unregistrations==1&&!mHandoffRegistered&&!RegisteredFreeze);}
  Fixture();TimerStatus=EFI_DEVICE_ERROR;assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_DEVICE_ERROR&&!mTimer&&!mExit);
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);
  assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_ALREADY_STARTED);
  TimerCallback((EFI_EVENT)1,NULL);assert(mActive&&Starts==1&&CrDpGopPublished()&&Installs==1&&Connects==1&&Allocations==1&&!Preserves);
  /* GraphicsConsole can keep the already-active GOP mode and only touch its
   * centered text window. Its untouched margins must start clean in DEBUG too. */
  for(UINTN I=0;I<mGopMode.FrameBufferSize;++I)assert(((UINT8 *)(UINTN)mGopMode.FrameBufferBase)[I]==0);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL White={.Blue=255,.Green=255,.Red=255};
  assert(mGop.Blt(&mGop,&White,EfiBltVideoFill,0,0,640,65,640,950,0)==EFI_SUCCESS);
  for(UINTN I=0;I<CR_DP_GOP_STRIDE*65;++I)assert(((UINT8 *)(UINTN)mGopMode.FrameBufferBase)[I]==0);
  for(UINTN I=CR_DP_GOP_STRIDE*1015;I<mGopMode.FrameBufferSize;++I)assert(((UINT8 *)(UINTN)mGopMode.FrameBufferBase)[I]==0);
  assert(MarkerReady&&SplitterReady&&mConsoleConnected&&ConsoleVariables[0]==3&&ConsoleVariables[1]==3&&
    ConsoleAppends[0]==1&&ConsoleAppends[1]==1);
  EFI_PHYSICAL_ADDRESS Buffer=mGopMode.FrameBufferBase;EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode=mGop.Mode;
  UINTN Flush=CacheFlushes,Bytes=FlushBytes;
  for(UINTN I=0;I<150;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(Starts==1&&Maps==1&&!Unmaps&&!Stops&&CacheFlushes==Flush&&FlushBytes==Bytes&&!SourceQueries);
  assert(ConsoleAppends[0]==1&&ConsoleAppends[1]==1);
  assert(LiveSnapshots==(COLOR_BARS_BUILD?3:0)&&LiveLinks==LiveSnapshots);GopPixels();
  assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED&&mActive&&!Stops);
  ++State.HpdIrqSequence;TimerCallback((EFI_EVENT)1,NULL);
  assert(Irqs==1&&Starts==1&&Maps==1&&!Unmaps&&!Stops&&mRoute.HpdIrqSequence==State.HpdIrqSequence);
  LiveLinkStatus=EFI_DEVICE_ERROR;++State.HpdIrqSequence;TimerCallback((EFI_EVENT)1,NULL);
  assert(Irqs==2&&Starts==2&&Maps==2&&Unmaps==1&&Allocations==1&&Installs==1&&mGopMode.FrameBufferBase==Buffer);
  LiveLinkStatus=EFI_SUCCESS;State.Hpd=FALSE;FailStop=TRUE;TimerCallback((EFI_EVENT)1,NULL);
  assert(!mActive&&mScanout&&ClockVotes==12&&Maps==Unmaps+1&&mGop.Mode==Mode&&mGopPages);GopPixels();
  UINTN Prior=Stops;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);assert(Stops==Prior&&mScanout);
  FailStop=FALSE;FailUnmap=TRUE;TimerCallback((EFI_EVENT)1,NULL);assert(!mScanout&&ClockVotes==12&&mGopPages);
  FailUnmap=FALSE;RetryCleanup();assert(!ClockVotes&&!PathVotes&&Maps==Unmaps&&mGop.Mode==Mode&&mGopMode.FrameBufferBase==Buffer);GopPixels();
  State.Hpd=TRUE;TimerCallback((EFI_EVENT)1,NULL);assert(mActive&&Starts==3&&Allocations==1&&Installs==1&&Connects==1);
  TypeCPublished=FALSE;TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&!mTypeC&&!ClockVotes&&Maps==Unmaps&&mGopPages);
  TypeCPublished=TRUE;TimerCallback((EFI_EVENT)1,NULL);assert(mActive&&Starts==4&&Allocations==1);
  ExitCallback((EFI_EVENT)2,NULL);assert(mExiting&&!mScanout&&mMapping&&mGopPages&&mGopMode.FrameBufferBase==Buffer);
  Flush=CacheFlushes;TimerCallback((EFI_EVENT)1,NULL);assert(Starts==4&&CacheFlushes==Flush);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixel={0};UINTN Size=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=NULL;
  assert(mGop.Blt(&mGop,&Pixel,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_NOT_READY);
  assert(mGop.QueryMode(&mGop,0,&Size,&Info)==EFI_NOT_READY&&MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);ConnectStatus=EFI_NOT_FOUND;
  TimerCallback((EFI_EVENT)1,NULL);Buffer=mGopMode.FrameBufferBase;
  assert(mActive&&CrDpGopPublished()&&Connects==1&&mConnectPending);
  for(UINTN I=0;I<9;++I)TimerCallback((EFI_EVENT)1,NULL);assert(Connects==1&&Installs==1&&Maps==1);
  ConnectStatus=EFI_SUCCESS;TimerCallback((EFI_EVENT)1,NULL);
  assert(Connects==2&&mConsoleConnected&&!mConnectPending&&mGopMode.FrameBufferBase==Buffer);RebootFixture();
  /* Actual Shell ConsoleLogger replaces only the system-table handle/protocol;
   * ConSplitter's permanent child and its forwarding output remain valid. */
  Fixture();SystemTable.ConsoleOutHandle=(EFI_HANDLE)0x5E11;
  assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  assert(SplitterReady&&mConsoleConnected&&Connects==1&&!mConnectPending);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);DelayText=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(Connects==1&&mActive&&!mConsoleConnected&&mConnectPending);
  DelayText=FALSE;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(Connects==2&&mConsoleConnected&&Installs==1&&Maps==1);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);ConnectStatus=EFI_DEVICE_ERROR;
  TimerCallback((EFI_EVENT)1,NULL);for(UINTN I=0;I<100;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(mActive&&Connects==6&&!mConnectPending&&Installs==1&&Maps==1);
  State.Hpd=FALSE;TimerCallback((EFI_EVENT)1,NULL);State.Hpd=TRUE;ConnectStatus=EFI_SUCCESS;TimerCallback((EFI_EVENT)1,NULL);
  assert(mActive&&Connects==7&&mConsoleConnected&&Allocations==1&&Installs==1);RebootFixture();
  /* Actual ConPlatform can produce SimpleTextOut without selecting it for
   * ConSplitter after BDS prunes an absent hot-plug controller. */
  Fixture();VariableStatus=EFI_WRITE_PROTECTED;
  assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  assert(mActive&&!Connects&&!mConsoleConnected&&mConnectPending&&ConsoleAppends[0]==1&&!ConsoleAppends[1]);
  TextReady=TRUE;assert(ConPlatformTextOutDriverBindingStart(&PlatformBinding,mGopHandle,NULL)==EFI_SUCCESS&&
    !MarkerReady&&!ConPlatformBound);
  VOID *UnselectedText=NULL;
  assert(ConSplitterStart(&SplitterBinding,mGopHandle,SystemTable.ConsoleOutHandle,&gEfiConsoleOutDeviceGuid,
    &gEfiSimpleTextOutProtocolGuid,&UnselectedText)==EFI_NOT_FOUND);
  VariableStatus=EFI_SUCCESS;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(mConsoleConnected&&MarkerReady&&SplitterReady&&ConsoleAppends[0]==2&&ConsoleAppends[1]==1&&
    ConsoleVariables[0]==3&&ConsoleVariables[1]==3&&Installs==1&&Maps==1);RebootFixture();
  Fixture();VariableStatus=EFI_WRITE_PROTECTED;VariableFailureType=ErrOut;
  assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  assert(!Connects&&mGopConOutAdded&&!mGopErrOutAdded&&ConsoleAppends[0]==1&&ConsoleAppends[1]==1);
  VariableStatus=EFI_SUCCESS;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(mConsoleConnected&&ConsoleAppends[0]==1&&ConsoleAppends[1]==2);RebootFixture();
  Fixture();DelayMarker=TRUE;assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  assert(TextReady&&!MarkerReady&&!mConsoleConnected&&mConnectPending);
  DelayMarker=FALSE;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(mConsoleConnected&&MarkerReady&&SplitterReady&&ConsoleAppends[0]==1&&ConsoleAppends[1]==1);RebootFixture();
  Fixture();DelaySplitter=TRUE;assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  assert(TextReady&&MarkerReady&&!SplitterReady&&!mConsoleConnected&&mConnectPending);
  DelaySplitter=FALSE;WrongSplitter=TRUE;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(SplitterReady&&!mConsoleConnected&&mConnectPending);
  WrongSplitter=FALSE;for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);
  assert(mConsoleConnected&&ConsoleAppends[0]==1&&ConsoleAppends[1]==1);RebootFixture();
  Fixture();ConsolePolicy=FALSE;assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);TimerCallback((EFI_EVENT)1,NULL);
  assert(mActive&&mGopPublished&&!Connects&&!ConsoleAppends[0]&&!ConsoleAppends[1]&&!mConsoleConnected);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);GcdCapabilities&=~EFI_MEMORY_WC;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&mReservationClaimAttempted&&!mMapping&&!CacheTransitions&&!Installs);
  GcdCapabilities|=EFI_MEMORY_WC;mRetryTicks=0;TimerCallback((EFI_EVENT)1,NULL);
  assert(mActive&&mGopCacheReady&&CacheTransitions==1);RebootFixture();
  CONST UINT64 Protected[]={EFI_MEMORY_RP,EFI_MEMORY_RO,EFI_MEMORY_WP};
  for(UINTN I=0;I<ARRAY_SIZE(Protected);++I){
    Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);GcdProtected=Protected[I];
    TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&mReservationClaimAttempted&&!Maps&&!CacheTransitions&&!CacheCleans&&!mBltConfig);
    assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS&&Allocations==Frees);
  }
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);GcdOverflow=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&!mReservationClaimAttempted&&!mGopPages&&!Maps&&!CacheTransitions&&!CacheCleans&&!mBltConfig);
  assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS&&Allocations==Frees);
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);FailCacheSet=CommitCacheSet=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&mGopPages&&!mMapping&&!mBltConfig&&mGopAttributesAttempted&&GcdAttributes==(EFI_MEMORY_WC|EFI_MEMORY_XP));
  FailCacheRestore=TRUE;assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_DEVICE_ERROR&&mGopPages&&!Frees);
  FailCacheRestore=FALSE;assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS&&Allocations==Frees);
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);BadCacheReadback=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&mGopPages&&!mMapping&&!mGopCacheReady);
  BadCacheReadback=FALSE;mRetryTicks=0;TimerCallback((EFI_EVENT)1,NULL);assert(mActive&&Allocations==1);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);FailAllocate=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&!mGopPages&&Maps==Unmaps&&!Installs);
  FailAllocate=FALSE;mRetryTicks=0;TimerCallback((EFI_EVENT)1,NULL);assert(mActive&&mGopPages);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);InstallStatus=EFI_DEVICE_ERROR;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&Maps==Unmaps&&mGopPages&&!CrDpGopPublished());
  FailFree=TRUE;assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_DEVICE_ERROR&&mGopPages&&mBltConfig);
  FailFree=FALSE;InstallStatus=EFI_SUCCESS;mRetryTicks=0;TimerCallback((EFI_EVENT)1,NULL);
  assert(!mActive&&!CrDpGopPublished()&&mReservationFreeAttempted);
  assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_SUCCESS&&Allocations==Frees);
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);InstallStatus=EFI_DEVICE_ERROR;CommitInstall=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&Maps==Unmaps&&mGopPages&&mGopPublicationPending);
  assert(MDSSDpCrDxeUnload((EFI_HANDLE)1)==EFI_ACCESS_DENIED);RebootFixture();
  Fixture();assert(MDSSDpCrDxeEntryPoint((EFI_HANDLE)1,NULL)==EFI_SUCCESS);FailPhyAcquire=FailPhyRelease=TRUE;
  TimerCallback((EFI_EVENT)1,NULL);assert(!mActive&&mPhyOwned&&PhyReleases==1&&mCleanupRetryTicks==10);
  for(UINTN I=0;I<10;++I)TimerCallback((EFI_EVENT)1,NULL);assert(PhyReleases==1);
  TimerCallback((EFI_EVENT)1,NULL);assert(PhyReleases==2&&mPhyOwned);
  FailPhyAcquire=FailPhyRelease=FALSE;RetryCleanup();assert(!mPhyOwned&&!PathVotes);
  mRetryTicks=0;TimerCallback((EFI_EVENT)1,NULL);assert(mActive);FailStop=TRUE;ExitCallback((EFI_EVENT)2,NULL);
  assert(mScanout&&mMapping&&mGopPages&&CrDpGopPublished());RebootFixture();
  HandoffLifecycle();Fixture();free(Backing);Backing=NULL;
  puts("DP actual GOP/BLT/handoff lifecycle: no source/copy timer, cache/publication, stable unplug/retrain buffer, immutable freeze/ownership snapshot and EBS retained failure PASS");return 0;
}
