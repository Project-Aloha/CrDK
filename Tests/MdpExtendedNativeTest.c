/* Complete native extended caller + actual private ClockLib service. GPL-2.0-only */
#define CR_MDSS_PANEL_FIXTURE_ONLY
#include "MdssDsiPanelTest.c"
#include "../Driver/MDSSCrDxe/MdpExtendedNative.h"
#include <Library/CrDalLib.h>
#include "../Target/Sm8450/CrMdssTarget.c"
static HW Hardware;
static PANEL_HW PanelHardware;
static UINT32 ProviderCalls,ProviderFail,FaultOnce,Maps,Unmaps,Grants,Frees,CacheFlushes;
static EFI_STATUS ProviderFailure;
static BOOLEAN RailHeld[2],PathHeld[2],Floor,CommitFault;
static int MapTokens[2];
static VOID *CommandAllocation;
static EFI_PHYSICAL_ADDRESS FramePhysical=0xb8000000;
static EFI_TPL CurrentTpl=TPL_APPLICATION;
#ifndef CR_MDP_NATIVE_FIXTURE_ONLY
EFI_BOOT_SERVICES *gBS;
EFI_GUID gEfiGraphicsOutputProtocolGuid;
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(VOID)Level;(VOID)Format;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(VOID)Level;return FALSE;}
#endif
/* The legacy SM8450 fixture must never enter the stopped-display ARC path.
 * Link its newer common-resource dependency and fail if routing changes. */
EFI_STATUS CrDalGetDeviceInfo(CR_DAL_DEVICE_TYPE Type,UINT32 Instance,UINT32 Revision,
 UINTN Bytes,CONST CR_DAL_DEVICE_INFO **Info){
 (VOID)Type;(VOID)Instance;(VOID)Revision;(VOID)Bytes;(VOID)Info;
 assert(0&&"unexpected stopped-display supplier in legacy fixture");return EFI_UNSUPPORTED;
}
EFI_STATUS NativeProviderStep(VOID){++ProviderCalls;EFI_STATUS S=ProviderCalls==ProviderFail?ProviderFailure:EFI_SUCCESS;
 if(S!=EFI_SUCCESS&&FaultOnce)ProviderFail=0;return S;}
VOID NativeClockFixtureSetup(EFI_CLOCK_CR_PROTOCOL **,EFI_CLOCK_EXTERNAL_CR_PROTOCOL **);
VOID NativeClockFixtureFinish(VOID);
BOOLEAN NativeClockFixtureOwned(VOID);
VOID NativeClockFixtureFail(UINT32,EFI_STATUS);
VOID NativeClockFixtureForeignIncarnation(VOID);
UINT32 EFIAPI ClockFixtureRead32(UINTN);
UINT32 EFIAPI ClockFixtureWrite32(UINTN,UINT32);
static CR_MDSS_EXT_WINDOW Window(UINTN A,UINT32 *O){
 const UINT64 B[]={0xae01000,0xae94000,0xae96000,0xae94400,0xae96400};
 const UINTN Size[]={0x8f000,0x400,0x400,0x800,0x800};
 for(UINT32 W=0;W<CrMdssExtWindows;++W)if(A>=B[W]&&A-B[W]<Size[W]){*O=(UINT32)(A-B[W]);return (CR_MDSS_EXT_WINDOW)W;}
 assert(0);return CrMdssExtWindows;
}
UINT32 EFIAPI MmioRead32(UINTN A){if(A>=0x30000000)return ClockFixtureRead32(A);
 UINT32 O,V;CR_MDSS_EXT_WINDOW W=Window(A,&O);assert(Read(&Hardware,W,O,&V)==CR_SUCCESS);return V;}
UINT32 EFIAPI MmioWrite32(UINTN A,UINT32 V){if(A>=0x30000000)return ClockFixtureWrite32(A,V);
 UINT32 O;CR_MDSS_EXT_WINDOW W=Window(A,&O);assert(Write(&Hardware,W,O,V)==CR_SUCCESS);return V;}
UINTN EFIAPI MicroSecondDelay(UINTN Us){assert(Us<=20000);return Us;}
VOID EFIAPI MemoryFence(VOID){}
VOID *EFIAPI ZeroMem(VOID *P,UINTN Bytes){return memset(P,0,Bytes);}
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN Bytes){assert(P&&Bytes);++CacheFlushes;return P;}
EFI_TPL EFIAPI EfiGetCurrentTpl(VOID){return CurrentTpl;}
static EFI_STATUS EFIAPI Gcd(UINT64 At,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
 EFI_STATUS S=NativeProviderStep();if(S!=EFI_SUCCESS)return S;memset(D,0,sizeof(*D));
 if(At>=FramePhysical&&At-FramePhysical<0x2b00000){D->BaseAddress=FramePhysical;D->Length=0x2b00000;D->GcdMemoryType=EfiGcdMemoryTypeReserved;}
 else {D->BaseAddress=0xae00000;D->Length=0x300000;D->GcdMemoryType=EfiGcdMemoryTypeMemoryMappedIo;}
 D->Attributes=EFI_MEMORY_WB;return EFI_SUCCESS;}
static EFI_STATUS EFIAPI Ensure(EFI_RPMH_CR_PROTOCOL *P){(void)P;EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS)Floor=TRUE;return S;}
static EFI_STATUS EFIAPI Rail(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *Name,UINT32 Mv,UINT8 Mode,BOOLEAN *Owned){
 (void)P;UINT32 I=!strcmp(Name,"ldob6")?0:1;assert(Floor&&Mv==(I?880U:1200U)&&Mode==7);
 EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS||CommitFault){assert(!RailHeld[I]);RailHeld[I]=TRUE;*Owned=TRUE;}return S;}
static EFI_STATUS EFIAPI RailOff(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *Name,BOOLEAN On){
 (void)P;assert(!On&&!NativeClockFixtureOwned()&&!PathHeld[0]&&!PathHeld[1]);UINT32 I=!strcmp(Name,"ldob6")?0:1;
 EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS){assert(RailHeld[I]);RailHeld[I]=FALSE;}return S;}
static EFI_STATUS EFIAPI Path(EFI_INTERCONNECT_CR_PROTOCOL *P,CONST CHAR8 *Compatible,UINT32 Source,UINT32 Dest,INTERCONNECT_PATH_HANDLE *Out){
 (void)P;assert(!strcmp(Compatible,"qcom,sm8450-mdp")&&RailHeld[0]&&RailHeld[1]);UINT32 I=Source==0x4002?0:1;
 assert(Dest==(I?0x4001U:0x4003U));EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS||CommitFault){PathHeld[I]=TRUE;*Out=I+1;}return S;}
static EFI_STATUS EFIAPI Bandwidth(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE Id,UINT64 Avg,UINT64 Peak){
 (void)P;assert(Id&&Id<=2&&PathHeld[Id-1]&&Avg&&Peak>=Avg);return NativeProviderStep();}
static EFI_STATUS EFIAPI PathOff(EFI_INTERCONNECT_CR_PROTOCOL *P,INTERCONNECT_PATH_HANDLE Id){
 (void)P;assert(Id&&Id<=2&&!NativeClockFixtureOwned());EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS){assert(PathHeld[Id-1]);PathHeld[Id-1]=FALSE;}return S;}
static EFI_STATUS EFIAPI Attach(EFI_SMMU_CR_PROTOCOL *P,UINT16 Segment,UINT16 Bdf,UINT16 Sid){
 (void)P;assert(!Segment&&Bdf==0xffff&&(Sid==0x2800||Sid==0x2802||Sid==0x2c00||Sid==0x2c02));assert(NativeClockFixtureOwned());return NativeProviderStep();}
static EFI_STATUS EFIAPI Allocate(EDKII_IOMMU_PROTOCOL *P,EFI_ALLOCATE_TYPE T,EFI_MEMORY_TYPE M,UINTN Pages,VOID **Out,UINT64 Attr){
 (void)P;assert(T==AllocateAnyPages&&M==EfiBootServicesData&&Pages==1&&!Attr&&!CommandAllocation);
 EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS||CommitFault){CommandAllocation=aligned_alloc(4096,4096);assert(CommandAllocation);*Out=CommandAllocation;}return S;}
static EFI_STATUS EFIAPI MapBuffer(EDKII_IOMMU_PROTOCOL *P,EDKII_IOMMU_OPERATION Op,VOID *Cpu,UINTN *Bytes,UINT64 *Iova,VOID **Token){
 (void)P;assert(NativeClockFixtureOwned());UINT32 I=Op==EdkiiIoMmuOperationBusMasterCommonBuffer?1:0;
 assert((I&&Cpu==CommandAllocation&&*Bytes==4096)||(!I&&Cpu==(VOID *)(UINTN)FramePhysical&&*Bytes==7680*1080));
 EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS||CommitFault){*Iova=I?0x20000000:0x10800000;*Token=&MapTokens[I];++Maps;}return S;}
static EFI_STATUS EFIAPI Grant(EDKII_IOMMU_PROTOCOL *P,UINT64 Base,UINT32 Sid,VOID *Token,UINT64 Access){
 (void)P;assert(Base==0x15000000&&(Token==&MapTokens[0]||Token==&MapTokens[1])&&Access==EDKII_IOMMU_ACCESS_READ);
 assert(Sid==0x2800||Sid==0x2802||Sid==0x2c00||Sid==0x2c02);EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS)++Grants;return S;}
static EFI_STATUS EFIAPI Unmap(EDKII_IOMMU_PROTOCOL *P,VOID *Token){
 (void)P;assert(Token==&MapTokens[0]||Token==&MapTokens[1]);assert(!(Hardware.R[CrMdssExtDsi0][1]&7)&&!(Hardware.R[CrMdssExtDsi1][1]&7));
 EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS)++Unmaps;return S;}
static EFI_STATUS EFIAPI Free(EDKII_IOMMU_PROTOCOL *P,UINTN Pages,VOID *Cpu){
 (void)P;assert(Pages==1&&Cpu==CommandAllocation);EFI_STATUS S=NativeProviderStep();if(S==EFI_SUCCESS){free(Cpu);CommandAllocation=NULL;++Frees;}return S;}
static EFI_RPMH_CR_PROTOCOL Rpmh={.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.AcquireVreg=Rail,.RpmhEnableVreg=RailOff,.EnsureBootFloor=Ensure};
static EFI_INTERCONNECT_CR_PROTOCOL Icb={EFI_INTERCONNECT_CR_PROTOCOL_REVISION,Path,Bandwidth,PathOff};
static EFI_SMMU_CR_PROTOCOL Smmu={.Revision=EFI_SMMU_CR_PROTOCOL_REVISION,.Attach=Attach};
static EDKII_IOMMU_PROTOCOL Iommu={.Map=MapBuffer,.Unmap=Unmap,.SetAttributeById=Grant,.AllocateBuffer=Allocate,.FreeBuffer=Free};
static EFI_DXE_SERVICES Dxe={.GetMemorySpaceDescriptor=Gcd};
static CR_MDSS_PANEL_PROFILE Setup(MDP_EXTENDED_PROVIDERS *P,MDP_EXTENDED_NATIVE_CONFIG *C,BOOLEAN Cphy,BOOLEAN Dsc){
 Reset(&Hardware);memset(&PanelHardware,0,sizeof(PanelHardware));PanelHardware.H=&Hardware;PanelHardware.Commit=TRUE;
 ProviderCalls=ProviderFail=Maps=Unmaps=Grants=Frees=CacheFlushes=0;FaultOnce=1;CommitFault=FALSE;ProviderFailure=EFI_DEVICE_ERROR;
 memset(RailHeld,0,sizeof(RailHeld));memset(PathHeld,0,sizeof(PathHeld));Floor=FALSE;assert(!CommandAllocation);
 NativeClockFixtureSetup(&P->Clock,&P->External);P->Rpmh=&Rpmh;P->Icb=&Icb;P->Smmu=&Smmu;P->Iommu=&Iommu;P->Dxe=&Dxe;
 *C=(MDP_EXTENDED_NATIVE_CONFIG){.Target=CrTargetGetMdssContext(),.MdpHz=200000000,.AverageKBps={1000,594000},.PeakKBps={1000,1188000},
  .Panel={&PanelHardware,PanelReady,PanelAcquire,PanelRelease,PanelReset,NULL,NULL}};
 CR_MDSS_PANEL_PROFILE F={.Revision=1,.Mode=Mode(Cphy,2,Cphy?2:4),.On=On,.Off=Off,.OnCount=ARRAY_SIZE(On),.OffCount=ARRAY_SIZE(Off),
  .Reset=Resets,.ResetCount=ARRAY_SIZE(Resets)};F.Mode.Dsc=Dsc;
 if(Dsc){F.Mode.SlicesPerPacket=1;CR_MDSS_DSC_PARAMS Q={.Revision=1,.MinorVersion=2,.BitsPerComponent=8,.BppQ4=128,
  .PictureWidth=1920,.PictureHeight=1080,.SliceWidth=480,.SliceHeight=8,.BlockPrediction=TRUE};assert(CrMdssDscBuild(&Q,&F.Mode.Compression)==CR_SUCCESS);}
 return F;
}
static UINT32 ActiveRetirement(BOOLEAN Cphy,BOOLEAN Dsc){
 MDP_EXTENDED_NATIVE N;MDP_EXTENDED_PROVIDERS P;MDP_EXTENDED_NATIVE_CONFIG C;UINT32 Cases=0;
 /* Fail real private-service access proof, a child plan's off operation, and
    final service release after active scanout; retained maps are never freed
    while an earlier stop phase remains incomplete. */
 const UINT32 Kinds[]={1,3,5};
 for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 I=0;I<ARRAY_SIZE(Kinds);++I){
  memset(&N,0,sizeof(N));CR_MDSS_PANEL_PROFILE F=Setup(&P,&C,Cphy,Dsc);
  assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)==EFI_SUCCESS);
  assert(MdpExtendedNativeStart(&N)==EFI_SUCCESS);
  NativeClockFixtureFail(Kinds[I],Positive?1:EFI_DEVICE_ERROR);
  assert(MdpExtendedNativeClose(&N)!=EFI_SUCCESS&&N.Initialized&&N.Closing);
  if(Kinds[I]!=5)assert(!Unmaps&&!Frees&&N.FrameMap&&N.CommandMap&&NativeClockFixtureOwned());
  else assert(Maps==Unmaps&&Frees==1&&N.ExternalOwned);
  assert(MdpExtendedNativeStart(&N)!=EFI_SUCCESS);
  assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&!N.Initialized&&Maps==Unmaps&&!CommandAllocation);
  NativeClockFixtureFinish();++Cases;
 }
 for(UINT32 Stuck=0;Stuck<2;++Stuck){
  memset(&N,0,sizeof(N));CR_MDSS_PANEL_PROFILE F=Setup(&P,&C,Cphy,Dsc);
  assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)==EFI_SUCCESS);
  assert(MdpExtendedNativeStart(&N)==EFI_SUCCESS);
  Hardware.StuckTiming=!Stuck;Hardware.StuckReset=Stuck;
  assert(MdpExtendedNativeClose(&N)==EFI_TIMEOUT&&N.Initialized&&N.Closing);
  assert(N.FrameMap&&N.CommandMap&&!Unmaps&&!Frees&&PanelHardware.Held&&NativeClockFixtureOwned());
  Hardware.StuckTiming=Hardware.StuckReset=FALSE;
  assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&!N.Initialized&&Maps==Unmaps);
  NativeClockFixtureFinish();++Cases;
 }
 /* A successful SetPlan with failed observation still retains the child
    lease and must be explicitly stopped; it may not publish an active mode. */
 for(UINT32 Kind=2;Kind<=4;++Kind){
  memset(&N,0,sizeof(N));CR_MDSS_PANEL_PROFILE F=Setup(&P,&C,Cphy,Dsc);
  assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)==EFI_SUCCESS);
  if(Kind==3){assert(P.External->Reserve(P.External,&N,&N.ExternalOwned)==EFI_SUCCESS);N.Incarnation=MAX_UINT64;
   NativeClockFixtureForeignIncarnation();}
  else NativeClockFixtureFail(Kind,EFI_DEVICE_ERROR);
  assert(MdpExtendedNativeStart(&N)!=EFI_SUCCESS&&!N.Display.Active&&N.Display.CleanupPending);
  assert(N.FrameMap&&N.CommandMap&&!Unmaps&&NativeClockFixtureOwned());
  assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&!N.Initialized&&Maps==Unmaps);
  NativeClockFixtureFinish();++Cases;
 }
 return Cases;
}
static UINT32 ExitFence(BOOLEAN Cphy,BOOLEAN Dsc){
 MDP_EXTENDED_NATIVE N;MDP_EXTENDED_PROVIDERS P;MDP_EXTENDED_NATIVE_CONFIG C;UINT32 Cases=0;
 for(UINT32 Failed=0;Failed<3;++Failed){
  memset(&N,0,sizeof(N));CR_MDSS_PANEL_PROFILE F=Setup(&P,&C,Cphy,Dsc);
  assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)==EFI_SUCCESS);
  assert(MdpExtendedNativeStart(&N)==EFI_SUCCESS);
  CurrentTpl=TPL_NOTIFY;
  if(Failed==1)Hardware.StuckTiming=TRUE;
  if(Failed==2)NativeClockFixtureFail(1,EFI_DEVICE_ERROR);
  EFI_STATUS S=MdpExtendedNativeFreeze(&N);
  assert((Failed?S!=EFI_SUCCESS:S==EFI_SUCCESS)&&N.Frozen&&!N.FreezeAccess);
  assert(N.FrameMap&&N.CommandMap&&!Unmaps&&!Frees&&PanelHardware.Held&&NativeClockFixtureOwned());
  Hardware.StuckTiming=FALSE;
  assert(MdpExtendedNativeFreeze(&N)==EFI_SUCCESS&&N.FreezeComplete);
  assert(!(mDpu[0x35000/4]&1)&&!(mDpu[0x36000/4]&1));
  assert(!(Hardware.R[CrMdssExtDsi0][1]&7)&&!(Hardware.R[CrMdssExtDsi1][1]&7));
  UINT32 Calls=ProviderCalls,Writes=Hardware.Writes;
  assert(MdpExtendedNativeFreeze(&N)==EFI_SUCCESS&&ProviderCalls==Calls&&Hardware.Writes==Writes);
  CurrentTpl=TPL_APPLICATION;
  assert(MdpExtendedNativeStart(&N)==EFI_ACCESS_DENIED&&MdpExtendedNativeClose(&N)==EFI_ACCESS_DENIED);
  assert(ProviderCalls==Calls&&Hardware.Writes==Writes&&Maps==2&&Unmaps==0);
  /* Fixture destruction only: firmware never reopens a frozen EBS owner. */
  N.Frozen=FALSE;
  assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&Maps==Unmaps);
  NativeClockFixtureFinish();++Cases;
 }
 return Cases;
}
#ifndef CR_MDP_NATIVE_FIXTURE_ONLY
int main(void){MDP_EXTENDED_NATIVE N;MDP_EXTENDED_PROVIDERS P;MDP_EXTENDED_NATIVE_CONFIG C;UINT32 Phases=0;
 memset(&N,0,sizeof(N));CR_MDSS_PANEL_PROFILE Identity=Setup(&P,&C,FALSE,FALSE);
 CR_MDSS_TARGET Wrong=*C.Target;Wrong.Soc=CR_MDSS_SOC_SM8350;C.Target=&Wrong;
 assert(MdpExtendedNativeOpen(&N,&P,&C,&Identity,0xb8000000,7680*1080)==EFI_INVALID_PARAMETER);
 assert(!ProviderCalls&&!Hardware.Writes&&!N.Initialized);NativeClockFixtureFinish();
 for(UINT32 Cphy=0;Cphy<2;++Cphy)for(UINT32 Dsc=0;Dsc<2;++Dsc){memset(&N,0,sizeof(N));CR_MDSS_PANEL_PROFILE F=Setup(&P,&C,Cphy,Dsc);
  EFI_STATUS Status=MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080);
  if(Status!=EFI_SUCCESS)fprintf(stderr,"native open CPHY%u DSC%u status%llx calls%u initialized%u clocks%u maps%u\n",Cphy,Dsc,(unsigned long long)Status,ProviderCalls,N.Initialized,NativeClockFixtureOwned(),Maps);
  assert(Status==EFI_SUCCESS);UINT32 Open=ProviderCalls;
  assert(N.FrameIova!=0xb8000000&&Maps==2&&Grants==8);
  assert(MdpExtendedNativeStart(&N)==EFI_SUCCESS&&N.Display.Active&&PanelHardware.Held&&CacheFlushes);
  assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&!N.Initialized&&!PanelHardware.Held&&Maps==Unmaps&&Frees==1);
  NativeClockFixtureFinish();
  for(UINT32 Committed=0;Committed<2;++Committed)for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 Fail=1;Fail<=Open;++Fail){memset(&N,0,sizeof(N));F=Setup(&P,&C,Cphy,Dsc);
   ProviderFail=Fail;CommitFault=Committed;ProviderFailure=Positive?1:EFI_DEVICE_ERROR;
   assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)!=EFI_SUCCESS);
   ProviderFail=0;assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&!N.Initialized&&Maps==Unmaps&&!CommandAllocation);
   NativeClockFixtureFinish();++Phases;
  }
  /* Off-path failures preserve state and permit exact retry, including after
     some clock reservations have already retired. */
  memset(&N,0,sizeof(N));F=Setup(&P,&C,Cphy,Dsc);assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)==EFI_SUCCESS);
  ProviderCalls=0;assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS);UINT32 Close=ProviderCalls;NativeClockFixtureFinish();
  for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 Fail=1;Fail<=Close;++Fail){memset(&N,0,sizeof(N));F=Setup(&P,&C,Cphy,Dsc);
   assert(MdpExtendedNativeOpen(&N,&P,&C,&F,0xb8000000,7680*1080)==EFI_SUCCESS);ProviderCalls=0;ProviderFail=Fail;ProviderFailure=Positive?1:EFI_DEVICE_ERROR;
   assert(MdpExtendedNativeClose(&N)!=EFI_SUCCESS&&N.Initialized&&N.Closing);
   ProviderFail=0;assert(MdpExtendedNativeStart(&N)!=EFI_SUCCESS);assert(MdpExtendedNativeClose(&N)==EFI_SUCCESS&&!N.Initialized);
   assert(Maps==Unmaps&&!CommandAllocation);NativeClockFixtureFinish();++Phases;
  }
  Phases+=ActiveRetirement(Cphy,Dsc);
  Phases+=ExitFence(Cphy,Dsc);
 }
 printf("Native real extended provider/core integration: dual DPHY/CPHY +/-DSC, separate DMA, %u retained provider phases PASS\n",Phases);return 0;
}
#endif
