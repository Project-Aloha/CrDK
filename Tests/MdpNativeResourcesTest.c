/* Actual shared native resource owner + retirement + typed SMMU composition.
 * Models suppliers, GCD, boot services and MMIO; no hardware evidence. MIT */
#define CR_SMMU_DISPLAY_TEST_NO_MAIN 1
#include "SmmuDisplayOwnershipTest.c"
#define mMdss mMdss8250
#define CrTargetGetMdssContext CrTestGetMdss8250
#include "../Target/Sm8250/CrMdssTarget.c"
#undef CrTargetGetMdssContext
#undef mMdss
#define mMdss mMdss8350
#define CrTargetGetMdssContext CrTestGetMdss8350
#include "../Target/Sm8350/CrMdssTarget.c"
#undef CrTargetGetMdssContext
#undef mMdss

#include "../Target/Sm8350/CrRpmhArcTarget.c"
STATIC CR_DAL_RPMH_ARC_ENTRY ArcEntries[7];
STATIC CR_DAL_RPMH_ARC_INVENTORY ArcInventory;
STATIC CR_DAL_DEVICE_INFO ArcInfo;
INTN EFIAPI AsciiStrCmp(CONST CHAR8 *A,CONST CHAR8 *B){return strcmp(A,B);}
EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE Type,UINT32 Instance,UINT32 Revision,
    UINTN Bytes,CONST CR_DAL_DEVICE_INFO **Info) {
  assert(Type==CrDalDeviceRpmhArcInventory&&!Instance&&Revision==CR_DAL_DATA_REVISION_1&&Bytes==sizeof(ArcInventory));
  if(DisplayTarget.Soc==0x8250){*Info=NULL;return EFI_NOT_FOUND;}
  *Info=&ArcInfo;return EFI_SUCCESS;
}
STATIC UINT32 MdpRead(UINTN A);
STATIC UINT32 MdpWrite(UINTN A,UINT32 V);
#define MicroSecondDelay MdpDelay
STATIC UINTN EFIAPI MdpDelay(UINTN Us);
#define MmioRead32 MdpRead
#define MmioWrite32 MdpWrite
#include "../Driver/MDSSCrDxe/MdpNativeResources.c"
#undef MmioRead32
#undef MmioWrite32
#undef MicroSecondDelay

STATIC UINT32 MdpRegs[CrMdssRetireWindows][0x90000/4];
STATIC UINTN ProviderCalls,ProviderFail,ProviderWrites,ProviderReleases,GcdCalls;
STATIC EFI_STATUS ProviderFailure;
STATIC EFI_TPL CurrentTpl;
STATIC BOOLEAN ResetOn,ExternalOn,FailRounded,GopPresent,ResetCompleted,PllPrepared,PllLost,InPllCallback;
STATIC CR_CLOCK_PLL_SUPPLY_CHECK PllSupplyCheck;
STATIC VOID *PllSupplyContext;
STATIC UINTN ResetHoldUs;
STATIC BOOLEAN OutputResetLow;
STATIC UINTN OutputBlankCalls;
STATIC UINT64 DisplayVote[2],ForeignVote[2];
STATIC UINT32 InfoCorruption;
STATIC MDP_NATIVE_RESOURCES Resources;
STATIC MDP_NATIVE_PROVIDERS Providers;
STATIC MDP_NATIVE_RESOURCE_CONFIG Config;
STATIC EFI_CLOCK_CR_PROTOCOL Clock;
STATIC EFI_CLOCK_EXTERNAL_CR_PROTOCOL External;
STATIC EFI_RPMH_CR_PROTOCOL Rpmh;
STATIC EFI_INTERCONNECT_CR_PROTOCOL Icb;
STATIC EFI_DXE_SERVICES ResourceDxe;

STATIC UINTN EFIAPI MdpDelay(UINTN Us){if(ResetOn)ResetHoldUs+=Us;return MicroSecondDelay(Us);}
STATIC EFI_STATUS ProviderStep(VOID){
  return ++ProviderCalls==ProviderFail?ProviderFailure:EFI_SUCCESS;
}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return CurrentTpl;}
STATIC EFI_TPL EFIAPI Raise(EFI_TPL T){EFI_TPL Old=CurrentTpl;CurrentTpl=T;return Old;}
STATIC VOID EFIAPI Restore(EFI_TPL T){CurrentTpl=T;}
EFI_GUID gEfiGraphicsOutputProtocolGuid=EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
STATIC EFI_STATUS EFIAPI Locate(EFI_GUID *G,VOID *Registration,VOID **P){
  assert(G==&gEfiGraphicsOutputProtocolGuid&&!Registration&&P);
  EFI_STATUS S=ProviderStep();if(S!=EFI_SUCCESS)return S;
  *P=GopPresent?&Resources:NULL;return GopPresent?EFI_SUCCESS:EFI_NOT_FOUND;
}
STATIC UINT32 *MdpAddress(UINTN A,CR_MDSS_RETIRE_WINDOW *Window,UINT32 *Offset){
  for(UINT32 W=0;W<CrMdssRetireWindows;++W){
    UINT64 B=Base(&Resources,(CR_MDSS_RETIRE_WINDOW)W);UINTN N=Size(&Resources,(CR_MDSS_RETIRE_WINDOW)W);
    if(A>=B&&A<B+N){*Window=(CR_MDSS_RETIRE_WINDOW)W;*Offset=(UINT32)(A-B);assert(!(*Offset&3));
      return &MdpRegs[W][*Offset/4];}
  }
  assert(0&&"unqualified MDSS access");return NULL;
}
STATIC UINT32 MdpRead(UINTN A){
  if(A==0xaf02000)return ResetOn?1:0;
  CR_MDSS_RETIRE_WINDOW W;UINT32 O;UINT32 *V=MdpAddress(A,&W,&O);
  assert(ResetCompleted&&Resources.FunctionalReady);
  if(W==CrMdssRetireDpu)assert(!ResetOn);
  return *V;
}
STATIC UINT32 MdpWrite(UINTN A,UINT32 V){
  assert(Resources.WritersSealed);CR_MDSS_RETIRE_WINDOW W;UINT32 O;
  *MdpAddress(A,&W,&O)=V;++ProviderWrites;
  if((W==CrMdssRetireDsi0||W==CrMdssRetireDsi1)&&O==0x118&&V){MdpRegs[W][1]&=~7U;MdpRegs[W][2]&=~6U;}
  if(W==CrMdssRetireVbif&&O==0x200)MdpRegs[W][0x204/4]=V;
  return V;
}
STATIC EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
  ++GcdCalls;EFI_STATUS S=ProviderStep();if(S!=EFI_SUCCESS)return S;memset(D,0,sizeof(*D));
  if(A>=FramePhysical&&A<FramePhysical+8192){
    D->BaseAddress=A&~(UINT64)EFI_PAGE_MASK;D->Length=EFI_PAGE_SIZE;
    D->GcdMemoryType=FailRounded&&A>=FramePhysical+EFI_PAGE_SIZE?EfiGcdMemoryTypeSystemMemory:EfiGcdMemoryTypeReserved;
  }else {D->BaseAddress=A;D->Length=0x100000;D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;}
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI SupplierClockAcquire(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 Hz,BOOLEAN *O){
  assert((PllPrepared&&ResetCompleted&&!ResetOn&&Resources.WritersSealed)&&"functional clocks must follow physical reset");
  assert(P==&Clock&&C&&N);(VOID)Hz;*O=TRUE;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI ClockSet(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,UINT64 Hz,BOOLEAN On){
  assert(P==&Clock&&C&&N&&!Hz&&!On);++ProviderReleases;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI OwnerReset(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G,BOOLEAN On){
  assert(OutputResetLow&&"panel must be blank before MDSS reset");
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration&&Resources.WritersSealed);
  if(ResetCompleted)return EFI_SUCCESS;
  if(!On)assert(ResetHoldUs>=20000);EFI_STATUS S=ProviderStep();if(S!=EFI_SUCCESS)return S;
  ResetOn=On;if(!On)ResetCompleted=TRUE;++ProviderWrites;if(On){memset(MdpRegs[CrMdssRetireDpu],0,sizeof(MdpRegs[0]));
    MdpRegs[CrMdssRetireDpu][0]=Config.Target->Soc==0x8250?0x60000000:0x70000000;}return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI OwnerResetCheck(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G){
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration);
  EFI_STATUS S=ProviderStep();return S==EFI_SUCCESS&&(!ResetCompleted||ResetOn)?EFI_NOT_READY:S;
}
STATIC EFI_STATUS EFIAPI FunctionalRetire(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G){
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration&&ResetCompleted&&!ResetOn);
  if(!Resources.FunctionalReady)for(UINT32 I=0;I<7;++I)assert(!Resources.ClockReady[I]||ResetCompleted);return ProviderStep();
}
STATIC EFI_STATUS EFIAPI PllPrepare(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G,
    CR_CLOCK_PLL_SUPPLY_CHECK Supply,VOID *Context) {
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration&&Context==O&&Supply);
  assert(Resources.IccResetStage==4&&DisplayVote[0]==Config.AverageKBps[0]&&DisplayVote[1]==Config.AverageKBps[1]);
  if(PllSupplyCheck)assert(PllSupplyCheck==Supply&&PllSupplyContext==Context);
  PllSupplyCheck=Supply;PllSupplyContext=Context;
  EFI_STATUS S=ProviderStep();if(S!=EFI_SUCCESS)return S;
  InPllCallback=TRUE;S=Supply(Context);InPllCallback=FALSE;
  if(S==EFI_SUCCESS)PllPrepared=TRUE;return S;
}
STATIC EFI_STATUS EFIAPI PllCheck(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G) {
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration);
  if(!PllPrepared||PllLost)return EFI_NOT_READY;
  EFI_STATUS S=ProviderStep();if(S!=EFI_SUCCESS)return S;
  InPllCallback=TRUE;S=PllSupplyCheck(PllSupplyContext);InPllCallback=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI ClockReset(EFI_CLOCK_CR_PROTOCOL *P,CONST CHAR8 *C,CONST CHAR8 *N,BOOLEAN A){
  (VOID)P;(VOID)C;(VOID)N;(VOID)A;assert(0&&"anonymous display reset forbidden");return EFI_ACCESS_DENIED;
}
STATIC EFI_STATUS EFIAPI Floor(EFI_RPMH_CR_PROTOCOL *P){assert(P==&Rpmh);return ProviderStep();}
STATIC EFI_STATUS EFIAPI RailAcquire(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *N,UINT32 Mv,UINT8 Mode,BOOLEAN *O){
  assert(P==&Rpmh&&N&&Mv&&Mode);*O=TRUE;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI RailRelease(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *N,BOOLEAN On){
  assert(P==&Rpmh&&N&&!On);++ProviderReleases;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI PathAcquire(EFI_INTERCONNECT_CR_PROTOCOL *P,CONST CHAR8 *N,UINT32 S,UINT32 D,INTERCONNECT_PATH_HANDLE *H){
  assert(P==&Icb&&N&&S!=D);*H=(INTERCONNECT_PATH_HANDLE)(UINTN)(S+1);return ProviderStep();
}
STATIC EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE H,UINT64 A,UINT64 B){
  assert(P==&Icb&&H&&B>=A);UINT32 I=H==(INTERCONNECT_PATH_HANDLE)(UINTN)(Config.Target->Paths[0].Source+1)?0:1;
  if(!A)assert(OutputResetLow&&ResetCompleted&&!ResetOn);DisplayVote[I]=A;
  assert(ForeignVote[0]==12345&&ForeignVote[1]==67890);return ProviderStep();
}
STATIC EFI_STATUS EFIAPI PathRelease(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE H){
  assert(P==&Icb&&H);++ProviderReleases;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI Reserve(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,BOOLEAN *Owned){
  assert(!InPllCallback&&P==&External&&O==&Resources);*Owned=TRUE;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI ExternalRetire(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G){
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration);EFI_STATUS S=ProviderStep();
  if(S==EFI_SUCCESS)ExternalOn=FALSE;return S;
}
STATIC EFI_STATUS EFIAPI ExternalCheck(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT64 G){
  assert(P==&External&&O==&Resources&&G==Resources.OwnerGeneration);EFI_STATUS S=ProviderStep();
  return S==EFI_SUCCESS&&ExternalOn?EFI_NOT_READY:S;
}
STATIC EFI_STATUS EFIAPI ExternalRelease(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O){
  assert(P==&External&&O==&Resources);++ProviderReleases;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI PlanSet(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT32 I,CONST CR_CLOCK_DSI_PLAN *C,BOOLEAN On,BOOLEAN *Owned){
  assert(P==&External&&O==&Resources&&I<2);(VOID)C;*Owned=On;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI PlanGet(EFI_CLOCK_EXTERNAL_CR_PROTOCOL *P,VOID *O,UINT32 I,CR_CLOCK_DSI_PLAN *C,BOOLEAN *On,BOOLEAN *Pending,UINT64 *G){
  assert(P==&External&&O==&Resources&&I<2);memset(C,0,sizeof(*C));*On=FALSE;*Pending=FALSE;*G=1;return ProviderStep();
}
STATIC EFI_STATUS EFIAPI GroupAcquire(EFI_SMMU_CR_PROTOCOL *P,UINT32 S,UINT64 G,EFI_SMMU_CR_DISPLAY_VERIFY_STOPPED V,VOID *C,EFI_SMMU_CR_DISPLAY_GROUP *O){
  EFI_STATUS R=ProviderStep();return R==EFI_SUCCESS?CrSmmuAcquireDisplayGroup(P,S,G,V,C,O):R;
}
STATIC EFI_STATUS EFIAPI FrameBind(EFI_SMMU_CR_PROTOCOL *P,EFI_SMMU_CR_DISPLAY_GROUP *G,VOID *C,EFI_PHYSICAL_ADDRESS A,UINTN N,EFI_SMMU_CR_DISPLAY_LEASE *L){
  EFI_STATUS S=ProviderStep();return S==EFI_SUCCESS?CrSmmuBindDisplayFrame(P,G,C,A,N,L):S;
}
STATIC EFI_STATUS EFIAPI CommandAllocate(EFI_SMMU_CR_PROTOCOL *P,EFI_SMMU_CR_DISPLAY_GROUP *G,UINTN N,EFI_SMMU_CR_DISPLAY_LEASE *L){
  EFI_STATUS S=ProviderStep();return S==EFI_SUCCESS?CrSmmuAllocateDisplayCommand(P,G,N,L):S;
}
STATIC EFI_STATUS EFIAPI BufferPrepare(EFI_SMMU_CR_PROTOCOL *P,EFI_SMMU_CR_DISPLAY_GROUP *G,EFI_SMMU_CR_DISPLAY_LEASE *L){
  EFI_STATUS S=ProviderStep();return S==EFI_SUCCESS?CrSmmuPrepareDisplayBuffer(P,G,L):S;
}
STATIC EFI_STATUS EFIAPI BufferCheck(EFI_SMMU_CR_PROTOCOL *P,EFI_SMMU_CR_DISPLAY_GROUP *G,EFI_SMMU_CR_DISPLAY_LEASE *L){
  EFI_STATUS S=ProviderStep();return S==EFI_SUCCESS?CrSmmuCheckDisplayBuffer(P,G,L):S;
}
STATIC EFI_STATUS EFIAPI FramePreserve(EFI_SMMU_CR_PROTOCOL *P,EFI_SMMU_CR_DISPLAY_GROUP *G,EFI_SMMU_CR_DISPLAY_LEASE *L){
  EFI_STATUS S=ProviderStep();return S==EFI_SUCCESS?CrSmmuPreserveDisplayFrame(P,G,L):S;
}
STATIC EFI_STATUS EFIAPI BufferQuery(EFI_SMMU_CR_PROTOCOL *P,EFI_SMMU_CR_DISPLAY_GROUP *G,EFI_SMMU_CR_DISPLAY_LEASE *L,EFI_SMMU_CR_DISPLAY_BUFFER_INFO *I){
  EFI_STATUS S=ProviderStep();if(S!=EFI_SUCCESS)return S;S=CrSmmuQueryDisplayBuffer(P,G,L,I);if(S!=EFI_SUCCESS)return S;
  if(InfoCorruption==1)++I->Revision;if(InfoCorruption==2)++I->Size;if(InfoCorruption==3)++I->OwnerGeneration;
  if(InfoCorruption==4)++I->Generation;if(InfoCorruption==5)I->GrantedMask=7;if(InfoCorruption==6)I->PageBytes-=EFI_PAGE_SIZE;
  if(InfoCorruption==7)I->Flags|=CR_SMMU_DISPLAY_CLEANUP_ONLY;if(InfoCorruption==8)I->LastStatus=EFI_WARN_UNKNOWN_GLYPH;
  if(InfoCorruption==9)++I->Bytes;if(InfoCorruption==10)++I->HostAddress;if(InfoCorruption==11)I->DeviceAddress=0x100000000ULL;
  if(InfoCorruption==12)I->Flags&=~CR_SMMU_DISPLAY_OWNER_READY;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI OrdinaryAttach(EFI_SMMU_CR_PROTOCOL *P,UINT16 S,UINT16 B,UINT16 I){
  (VOID)P;(VOID)S;(VOID)B;(VOID)I;assert(0&&"ordinary Attach forbidden");return EFI_UNSUPPORTED;
}
STATIC EFI_STATUS BlankOutput(VOID *C){
  assert(C==&Resources&&!ResetOn&&!ResetCompleted&&!ProviderWrites&&!Resources.FunctionalReady);
  ++OutputBlankCalls;
  /* GPIO can take effect before its readback fails. Such failure must still
   * prevent MDSS reset and be retried with the same output owner. */
  OutputResetLow=TRUE;return ProviderStep();
}
STATIC VOID RebootFixture(UINT32 Soc){
  /* Test-only simulated reboot discards permanent journals. This is never
     claimed as resource cleanup and has no production reset API. */
  Fixture(Soc);if(Soc==0x8250)DisabledEmptyFixture(Soc,4,4);BuffersFixture();mOwner=NULL;mGeneration=0;memset(&Resources,0,sizeof(Resources));
  DisplayTarget=*(Soc==0x8250?CrTestGetMdss8250():CrTestGetMdss8350());
  memset(MdpRegs,0,sizeof(MdpRegs));MdpRegs[CrMdssRetireDpu][0]=Soc==0x8250?0x60000000:0x70000000;
  for(UINT32 I=CrMdssRetireDsi0;I<=CrMdssRetireDsi1;++I){MdpRegs[I][1]=7;MdpRegs[I][2]=6;}
  for(UINT32 I=CrMdssRetirePhy0;I<=CrMdssRetirePhy1;++I){MdpRegs[I][0x3c/4]=1;MdpRegs[I][0x24/4]=0x7f;MdpRegs[I][0x14/4]=0x20;MdpRegs[I][0x1c/4]=0xff;}
  assert(MockAllocatePages(AllocateAnyPages,EfiReservedMemoryType,2,&FramePhysical)==EFI_SUCCESS);
  ProviderCalls=ProviderFail=ProviderWrites=ProviderReleases=GcdCalls=0;ProviderFailure=EFI_DEVICE_ERROR;
  OutputResetLow=FALSE;OutputBlankCalls=0;
  memcpy(ArcEntries,mSm8350ArcEntries,sizeof(ArcEntries));
  ArcInventory=mSm8350ArcInventory;ArcInventory.Entries=ArcEntries;
  ArcInfo=(CR_DAL_DEVICE_INFO){.DataRevision=CR_DAL_DATA_REVISION_1,.DataSize=sizeof(ArcInventory),.Data=&ArcInventory};
  PllPrepared=PllLost=InPllCallback=FALSE;PllSupplyCheck=NULL;PllSupplyContext=NULL;
  CurrentTpl=TPL_APPLICATION;ResetOn=ResetCompleted=FALSE;ResetHoldUs=0;DisplayVote[0]=DisplayVote[1]=0;ForeignVote[0]=12345;ForeignVote[1]=67890;ExternalOn=TRUE;FailRounded=GopPresent=FALSE;InfoCorruption=0;
  gBS->LocateProtocol=Locate;gBS->RaiseTPL=Raise;gBS->RestoreTPL=Restore;
  Clock=(EFI_CLOCK_CR_PROTOCOL){.Revision=EFI_CLOCK_CR_PROTOCOL_REVISION,.AcquireClock=SupplierClockAcquire,.SetClock=ClockSet,.SetReset=ClockReset};
  Rpmh=(EFI_RPMH_CR_PROTOCOL){.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.AcquireVreg=RailAcquire,.RpmhEnableVreg=RailRelease,.EnsureBootFloor=Floor};
  Icb=(EFI_INTERCONNECT_CR_PROTOCOL){.Revision=EFI_INTERCONNECT_CR_PROTOCOL_REVISION,.AcquirePath=PathAcquire,.SetBandwidth=Bandwidth,.ReleasePath=PathRelease};
  External=(EFI_CLOCK_EXTERNAL_CR_PROTOCOL){.Revision=EFI_CLOCK_EXTERNAL_CR_PROTOCOL_REVISION,.Reserve=Reserve,.SetPlan=PlanSet,.GetPlan=PlanGet,.ReleaseOwner=ExternalRelease,.RetireInheritedDsi=ExternalRetire,.CheckInheritedDsiOff=ExternalCheck,.ResetCore=OwnerReset,.CheckCoreResetReleased=OwnerResetCheck,.RetireInheritedFunctional=FunctionalRetire,.PrepareFunctionalPll=PllPrepare,.CheckFunctionalPll=PllCheck};
  ResourceDxe=(EFI_DXE_SERVICES){.GetMemorySpaceDescriptor=Descriptor};
  mCrProtocol.Revision=EFI_SMMU_CR_PROTOCOL_REVISION;
  mCrProtocol.AcquireDisplayGroup=GroupAcquire;mCrProtocol.BindDisplayFrame=FrameBind;mCrProtocol.AllocateDisplayCommand=CommandAllocate;
  mCrProtocol.PrepareDisplayBuffer=BufferPrepare;mCrProtocol.CheckDisplayBuffer=BufferCheck;mCrProtocol.QueryDisplayBuffer=BufferQuery;
  mCrProtocol.PreserveDisplayFrame=FramePreserve;mCrProtocol.Attach=OrdinaryAttach;
  Providers=(MDP_NATIVE_PROVIDERS){.Clock=&Clock,.External=&External,.Rpmh=&Rpmh,.Icb=&Icb,.Smmu=&mCrProtocol,.Iommu=&mIoMmuProtocol,.Dxe=&ResourceDxe};
  Config=(MDP_NATIVE_RESOURCE_CONFIG){.Target=&DisplayTarget,.MdpHz=400000000,.AverageKBps={1000,2000},.PeakKBps={4000,8000},
    .BlankOutput=BlankOutput,.OutputContext=&Resources};
}
STATIC EFI_STATUS OpenResources(VOID){return MdpNativeResourcesOpen(&Resources,&Providers,&Config,1,FramePhysical,6000);}
int main(VOID){
  UINTN Cases=0;CONST UINT32 Socs[]={0x8250,0x8350};
  for(UINT32 So=0;So<2;++So){UINT32 Soc=Socs[So];RebootFixture(Soc);
    EFI_STATUS Initial=OpenResources();
    if(Initial!=EFI_SUCCESS)fprintf(stderr,"Open SoC=%x status=%llx stage=%u retirement=%u calls=%llu group=%llx\n",Soc,(unsigned long long)Initial,Resources.Stage,Resources.Retirement.Stage,(unsigned long long)ProviderCalls,(unsigned long long)mDisplayGroup.LastStatus);
    assert(Initial==EFI_SUCCESS&&PllPrepared&&Resources.AccessReady&&Resources.Retirement.Resumed);
    assert(OutputBlankCalls==1&&OutputResetLow);
    if(Soc==0x8250)assert(Hardware[HW_CB(4)/4]==0&&Hardware[(0xc00+4*4)/4]==0x20000);
    UINTN Calls=ProviderCalls,Stops=StopChecks,ResetWrites=ProviderWrites;
    assert(CrMdssRetireCheck(&Resources.Retirement)==CR_NOT_READY);
    assert(MdpNativeResourcesReady(&Resources)==EFI_SUCCESS&&StopChecks==Stops);
    assert(MdpNativeResourcesPublishFrame(&Resources,Resources.FrameCpu,6000)==EFI_SUCCESS);
    assert(MdpNativeResourcesPublishCommand(&Resources,Resources.CommandCpu,256)==EFI_SUCCESS);
    assert(ProviderWrites==ResetWrites&&Hardware[HW_CB(Soc==0x8250?4:3)/4]==0);
    for(UINT32 I=0;I<4;++I){CR_SMMU_DOMAIN *D=FindDomainBySid(DisplayTarget.Streams[I]);assert(D&&D->Domain.Stream!=(Soc==0x8250?4:3)&&D->Domain.Bank!=(Soc==0x8250?4:3));}
    /* Actual typed SMMU query/check, not a cached READY stub, at console TPL. */
    CurrentTpl=TPL_NOTIFY;UINTN NotifyWrites=ProviderWrites;
    assert(MdpNativeResourcesSuppliersReady(&Resources)==EFI_SUCCESS);
    assert(MdpNativeResourcesCheck(&Resources)==EFI_SUCCESS);
    assert(MdpNativeResourcesPublishFrame(&Resources,Resources.FrameCpu,6000)==EFI_SUCCESS);
    assert(ProviderWrites==NotifyWrites&&CurrentTpl==TPL_NOTIFY);
    assert(OpenResources()==EFI_ACCESS_DENIED&&MdpNativeResourcesClose(&Resources)==EFI_ACCESS_DENIED);
    CurrentTpl=TPL_HIGH_LEVEL;assert(MdpNativeResourcesSuppliersReady(&Resources)==EFI_ACCESS_DENIED);
    CurrentTpl=TPL_APPLICATION;
    UINTN OldWrites=ProviderWrites;PllLost=TRUE;
    assert(MdpNativeResourcesReady(&Resources)==EFI_NOT_READY);
    assert(MdpNativeResourcesWrite(&Resources,CrMdssRetireDsi0,0,1)==EFI_NOT_READY&&ProviderWrites==OldWrites);
    PllLost=FALSE;
    MDP_NATIVE_RESOURCES Copy=Resources;assert(MdpNativeResourcesReady(&Copy)!=EFI_SUCCESS);
    assert(MdpNativeResourcesOpen(&Copy,&Providers,&Config,1,FramePhysical,6000)==EFI_ACCESS_DENIED);
    UINTN Free=mFreeCalls;assert(MdpNativeResourcesClose(&Resources)==EFI_UNSUPPORTED&&mFreeCalls==Free&&mOwner==&Resources);
    for(UINT32 Warning=0;Warning<2;++Warning)for(UINTN Fail=1;Fail<=Calls;++Fail){
      RebootFixture(Soc);ProviderFail=Fail;ProviderFailure=Warning?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;
      assert(OpenResources()!=EFI_SUCCESS&&!Resources.AccessReady);ProviderFail=0;
      Free=mFreeCalls;
      if(Resources.OutputTouched||Resources.Retirement.Touched||Resources.Group.Token){
        assert(MdpNativeResourcesClose(&Resources)==EFI_UNSUPPORTED&&mOwner==&Resources&&mFreeCalls==Free);
        assert(OpenResources()==EFI_SUCCESS&&Resources.AccessReady);
      }else{
        if(Resources.Initialized)assert(OpenResources()==EFI_SUCCESS&&Resources.AccessReady);
        else assert(MdpNativeResourcesClose(&Resources)==EFI_SUCCESS&&!mOwner&&mFreeCalls==Free);
      }
      ++Cases;
    }
    for(UINT32 Bad=1;Bad<=12;++Bad){RebootFixture(Soc);InfoCorruption=Bad;
      assert(OpenResources()!=EFI_SUCCESS&&!Resources.AccessReady&&Resources.Stage==8);InfoCorruption=0;
      assert(OpenResources()==EFI_SUCCESS);++Cases;}
    for(UINT32 Bad=0;Bad<(Soc==0x8350?6U:2U);++Bad){RebootFixture(Soc);
      if(Bad==0)External.PrepareFunctionalPll=NULL;
      if(Bad==1)External.CheckFunctionalPll=NULL;
      if(Bad==2)ArcEntries[5].MinLevel=64;
      if(Bad==3)ArcEntries[5].ParentIndex=0;
      if(Bad==4)ArcEntries[1].MinLevel=64;
      if(Bad==5)ArcEntries[6].Kind=CR_DAL_RPMH_ARC_POWER;
      assert(OpenResources()!=EFI_SUCCESS&&!Resources.AccessReady&&!Resources.FunctionalReady&&!HardwareWrites);
      for(UINT32 I=0;I<7;++I)assert(!Resources.ClockOwned[I]);++Cases;
    }
    RebootFixture(Soc);FailRounded=TRUE;
    assert(OpenResources()!=EFI_SUCCESS&&Resources.Stage==1&&!ProviderWrites&&!HardwareWrites);
    assert(MdpNativeResourcesClose(&Resources)==EFI_SUCCESS&&!mOwner);
    RebootFixture(Soc);GopPresent=TRUE;
    assert(OpenResources()==EFI_ACCESS_DENIED&&!Resources.Initialized&&!ProviderWrites&&!HardwareWrites);
    RebootFixture(Soc);Providers.Smmu->Revision=EFI_SMMU_CR_DISPLAY_OWNERSHIP_REVISION-1;
    assert(OpenResources()==EFI_INVALID_PARAMETER&&!ProviderCalls&&!Resources.Initialized);
    Providers.Smmu->Revision=EFI_SMMU_CR_PROTOCOL_REVISION;
    printf("Native MDSS resources SoC=%x: actual resumed retirement + typed SMMU, %zu acquisition failure boundaries PASS\n",Soc,(size_t)Calls);
  }
  printf("Native shared MDSS actual composition: %zu error/warning/schema cases PASS; modeled suppliers/MMIO only\n",(size_t)Cases);return 0;
}
