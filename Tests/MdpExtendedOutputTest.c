/* Complete actual GOP/display/private-clock integration. GPL-2.0-only */
#include <stdarg.h>
#define CR_MDP_NATIVE_FIXTURE_ONLY
#define WriteBackDataCacheRange NativeCacheFlush
#include "MdpExtendedNativeTest.c"
#undef WriteBackDataCacheRange
#include "../Driver/MDSSCrDxe/MdpExtendedOutput.h"
static EFI_BOOT_SERVICES Boot;
EFI_BOOT_SERVICES *gBS=&Boot;
EFI_GUID gEfiEventExitBootServicesGuid,gEfiDevicePathProtocolGuid,gEfiGraphicsOutputProtocolGuid,gEfiDisplayPowerStateProtocolGuid;
static MDP_EXTENDED_OUTPUT O;
static EFI_MDSS_HANDOFF_FREEZE OwnerFreeze;
static EFI_MDSS_HANDOFF_SNAPSHOT OwnerSnapshot;
static VOID *OwnerContext;
static EFI_EVENT_NOTIFY ExitCallback;
static VOID *ExitContext;
static UINT64 FrameAttributes;
static UINT32 OutputCalls,OutputFail,OutputFail2,OutputCloses,OutputInstalls,PoolCount;
static EFI_STATUS OutputFailure;
static BOOLEAN CommitOutput,FreezeOnAllocation,FreezeOnFlush,NullPublication;
static EFI_STATUS OutputStep(VOID){++OutputCalls;return OutputCalls==OutputFail||OutputCalls==OutputFail2?OutputFailure:EFI_SUCCESS;}
static EFI_TPL EFIAPI Raise(EFI_TPL T){assert(T>=CurrentTpl);EFI_TPL Old=CurrentTpl;CurrentTpl=T;return Old;}
static VOID EFIAPI Restore(EFI_TPL T){assert(T<=CurrentTpl);CurrentTpl=T;}
static EFI_STATUS EFIAPI Owner(EFI_MDSS_HANDOFF_PROTOCOL *H,UINT32 Id,VOID *C,
  EFI_MDSS_HANDOFF_FREEZE F,EFI_MDSS_HANDOFF_SNAPSHOT S){
 (void)H;assert(Id==CR_MDSS_NATIVE_DSI&&C==&O);EFI_STATUS E=OutputStep();if(E!=EFI_SUCCESS)return E;
 if(F){assert(!OwnerContext&&F&&S);OwnerContext=C;OwnerFreeze=F;OwnerSnapshot=S;}
 else{assert(OwnerContext==C);OwnerContext=NULL;OwnerFreeze=NULL;OwnerSnapshot=NULL;}return EFI_SUCCESS;
}
static EFI_MDSS_HANDOFF_PROTOCOL Handoff={EFI_MDSS_HANDOFF_PROTOCOL_REVISION,Owner};
static EFI_STATUS EFIAPI Create(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY F,CONST VOID *C,
  CONST EFI_GUID *G,EFI_EVENT *E){
 assert(Type==EVT_NOTIFY_SIGNAL&&T==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid&&OwnerContext==C);
 EFI_STATUS S=OutputStep();if(S==EFI_SUCCESS||CommitOutput){*E=(VOID *)0x111;ExitCallback=F;ExitContext=(VOID *)C;}return S;
}
static EFI_STATUS EFIAPI Close(EFI_EVENT E){assert(E==(VOID *)0x111);++OutputCloses;
 EFI_STATUS S=OutputStep();if(S==EFI_SUCCESS){ExitCallback=NULL;ExitContext=NULL;}return S;
}
static EFI_STATUS EFIAPI GetDescriptor(UINT64 At,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D){
 EFI_STATUS S=Gcd(At,D);if(S!=EFI_SUCCESS)return S;
 if(At==FramePhysical){S=OutputStep();if(S!=EFI_SUCCESS)return S;D->Attributes=FrameAttributes;D->Capabilities=EFI_MEMORY_WC|EFI_MEMORY_WB;}
 return S;
}
static EFI_STATUS EFIAPI Attributes(UINT64 Base,UINT64 Bytes,UINT64 Attr){
 assert(Base==FramePhysical&&Bytes==7680*1080);assert(!O.Native.Display.Active);
 EFI_STATUS S=OutputStep();if(S==EFI_SUCCESS||CommitOutput)FrameAttributes=Attr;return S;
}
static EFI_STATUS EFIAPI Install(EFI_HANDLE *Handle,...){
 ++OutputInstalls;assert(O.Native.Display.Active&&OwnerContext==&O&&ExitCallback&&O.CacheReady&&O.BltConfig);
 UINT32 Count=0;EFI_GUID *G;VOID *P;va_list A;va_start(A,Handle);
 while((G=va_arg(A,EFI_GUID *))){P=va_arg(A,VOID *);++Count;
  if(G==&gEfiDevicePathProtocolGuid)assert(P==&O.Path);
  else if(G==&gEfiGraphicsOutputProtocolGuid)assert(P==&O.Gop&&O.Gop.Mode->Info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor);
  else if(G==&gEfiDisplayPowerStateProtocolGuid)assert(P==&O.Power);else assert(0);
 }va_end(A);assert(Count==3);EFI_STATUS S=OutputStep();if((S==EFI_SUCCESS||CommitOutput)&&!NullPublication)*Handle=(VOID *)0x222;return S;
}
static VOID ForceFreeze(VOID){EFI_TPL T=Raise(TPL_NOTIFY);
 assert(OwnerFreeze&&OwnerFreeze(&O)==EFI_NOT_READY&&O.Exiting&&!O.Frozen);Restore(T);}
VOID *EFIAPI AllocatePool(UINTN Bytes){EFI_STATUS S=OutputStep();if(S!=EFI_SUCCESS)return NULL;
 VOID *P=malloc(Bytes);if(P)++PoolCount;return P;}
VOID *EFIAPI AllocateCopyPool(UINTN Bytes,CONST VOID *From){VOID *P=AllocatePool(Bytes);if(P)memcpy(P,From,Bytes);
 if(FreezeOnAllocation){FreezeOnAllocation=FALSE;ForceFreeze();}return P;}
VOID EFIAPI FreePool(VOID *P){assert(P&&PoolCount);--PoolCount;free(P);}
VOID *EFIAPI WriteBackInvalidateDataCacheRange(VOID *P,UINTN Bytes){assert(P==(VOID *)(UINTN)FramePhysical&&Bytes==7680*1080);return P;}
/* MdpExtendedNativeTest provides the actual publication mock. Rename it in
   that include to observe a freeze dispatched during this caller's row flush. */
VOID *EFIAPI WriteBackDataCacheRange(VOID *P,UINTN Bytes){
 assert(P&&Bytes);++CacheFlushes;
 if(FreezeOnFlush){FreezeOnFlush=FALSE;ForceFreeze();}return P;
}
VOID *EFIAPI SetMem32(VOID *P,UINTN N,UINT32 V){assert(!(N&3));for(UINTN I=0;I<N;I+=4)memcpy((UINT8 *)P+I,&V,4);return P;}
VOID *EFIAPI SetMem64(VOID *P,UINTN N,UINT64 V){assert(!(N&7));for(UINTN I=0;I<N;I+=8)memcpy((UINT8 *)P+I,&V,8);return P;}
INTN EFIAPI HighBitSet32(UINT32 V){if(!V)return -1;INTN N=0;while(V>>=1)++N;return N;}
VOID EFIAPI DebugPrint(UINTN Level,CONST CHAR8 *Format,...){(void)Level;(void)Format;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN Level){(void)Level;return FALSE;}
BOOLEAN EFIAPI DebugCodeEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugAssertEnabled(VOID){return FALSE;}
VOID EFIAPI DebugAssert(CONST CHAR8 *File,UINTN Line,CONST CHAR8 *Text){fprintf(stderr,"assert %s:%llu %s\n",File,(unsigned long long)Line,Text);abort();}
static CR_MDSS_PANEL_PROFILE OutputSetup(MDP_EXTENDED_PROVIDERS *P,MDP_EXTENDED_NATIVE_CONFIG *C,BOOLEAN Cphy,BOOLEAN Dsc){
 assert(!OwnerContext&&!ExitCallback&&!PoolCount);memset(&O,0,sizeof(O));
 CR_MDSS_PANEL_PROFILE F=Setup(P,C,Cphy,Dsc);Dxe.GetMemorySpaceDescriptor=GetDescriptor;Dxe.SetMemorySpaceAttributes=Attributes;
 FrameAttributes=EFI_MEMORY_WB|EFI_MEMORY_XP;OutputCalls=OutputFail=OutputFail2=OutputCloses=OutputInstalls=0;
 OutputFailure=EFI_DEVICE_ERROR;CommitOutput=FALSE;FreezeOnAllocation=FreezeOnFlush=NullPublication=FALSE;
 Boot=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.CreateEventEx=Create,.CloseEvent=Close,.InstallMultipleProtocolInterfaces=Install};
 return F;
}
static VOID RebootFixture(VOID){
 /* Destroy retained boot-only storage after a simulated reset; no production
    path withdraws published GOPs or reopens EBS admission. */
 O.Published=O.PublicationPending=O.Exiting=O.Frozen=FALSE;O.Native.Frozen=FALSE;
 assert(MdpExtendedOutputClose(&O)==EFI_SUCCESS&&!O.Initialized&&!OwnerContext&&!ExitCallback&&!PoolCount);
 assert(Maps==Unmaps&&!CommandAllocation);NativeClockFixtureFinish();
}
/* GraphicsConsole clear/cursor and MU SRE call the real GOP at TPL_NOTIFY. */
static VOID CheckNotifyGop(VOID){
 EFI_TPL Old=Raise(TPL_NOTIFY);
 UINTN Size=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=NULL;
 assert(O.Gop.QueryMode(&O.Gop,0,&Size,&Info)==EFI_SUCCESS&&Size==sizeof(*Info));
 assert(Info!=&O.Info&&Info->HorizontalResolution==1920);FreePool(Info);
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={.Blue=3,.Green=5,.Red=7},Pixels[4];
 assert(O.Gop.Blt(&O.Gop,&Color,EfiBltVideoFill,0,0,2,3,2,2,0)==EFI_SUCCESS);
 assert(O.Gop.Blt(&O.Gop,Pixels,EfiBltVideoToBltBuffer,2,3,0,0,2,2,0)==EFI_SUCCESS);
 for(UINT32 I=0;I<4;++I)assert(!memcmp(Pixels+I,&Color,4));
 assert(O.Gop.Blt(&O.Gop,Pixels,EfiBltBufferToVideo,0,0,4,5,2,2,0)==EFI_SUCCESS);
 assert(O.Gop.Blt(&O.Gop,NULL,EfiBltVideoToVideo,4,5,6,7,2,2,0)==EFI_SUCCESS);
 assert(O.Gop.SetMode(&O.Gop,0)==EFI_SUCCESS&&CurrentTpl==TPL_NOTIFY);
 assert(O.Gop.QueryMode(&O.Gop,1,&Size,&Info)==EFI_UNSUPPORTED);
 assert(O.Gop.SetMode(&O.Gop,1)==EFI_UNSUPPORTED);
 /* Supporting GOP drawing must not admit power/lifetime mutations here. */
 assert(O.Power.SetDisplayPowerState(&O.Power,EfiDisplayPowerStateOn)==EFI_ACCESS_DENIED);
 O.Busy=TRUE;assert(O.Gop.Blt(&O.Gop,&Color,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_NOT_READY);O.Busy=FALSE;
 EFI_TPL High=Raise(TPL_HIGH_LEVEL);UINT32 Calls=OutputCalls,Flushes=CacheFlushes;
 assert(O.Gop.QueryMode(&O.Gop,0,&Size,&Info)==EFI_ACCESS_DENIED);
 assert(O.Gop.SetMode(&O.Gop,0)==EFI_ACCESS_DENIED);
 assert(O.Gop.Blt(&O.Gop,&Color,EfiBltVideoFill,0,0,0,0,1,1,0)==EFI_ACCESS_DENIED);
 assert(OutputCalls==Calls&&CacheFlushes==Flushes&&CurrentTpl==TPL_HIGH_LEVEL);
 Restore(High);Restore(Old);
}
static VOID ValidateSnapshot(CONST CR_MDSS_HANDOFF_PARTICIPANT *Snapshot){
 for(UINT32 First=0;First<2;++First)for(UINT32 Revoked=0;Revoked<2;++Revoked){
  CR_MDSS_NATIVE_HANDOFF Record={0};CR_MDSS_HANDOFF_PARTICIPANT Mapping={.Owner=CR_MDSS_NATIVE_SMMU};
  assert(CrMdssHandoffInit(&Record,1,2,0xfc400000,8192,31,31)==CR_SUCCESS);
  Mapping.Mappings[0]=(CR_MDSS_NATIVE_MAPPING){.Owner=CR_MDSS_NATIVE_DSI,.MappingId=1,
   .State=Revoked?CR_MDSS_NATIVE_MAPPING_REVOKED:CR_MDSS_NATIVE_MAPPING_UNKNOWN,
   .Status=Revoked?0:EFI_NOT_READY,.StreamCount=Revoked?4:0};
  Mapping.Mappings[1]=(CR_MDSS_NATIVE_MAPPING){.Owner=CR_MDSS_NATIVE_DP,.State=CR_MDSS_NATIVE_MAPPING_NONE};
  const UINT16 Sids[]={0x2800,0x2802,0x2c00,0x2c02};
  if(Revoked)for(UINT32 I=0;I<4;++I){SmmuMappingEvidence *E=&Mapping.Mappings[0].Streams[I];
   *E=(SmmuMappingEvidence){.Base=0x15000000,.BankBase=0x15080000+I*4096,
    .Tables=0xfe000000+I*0x22000,.TableBytes=0x22000,.Iova=Snapshot->Route.DeviceAddress,
    .Bytes=8294400,.Sid=Sids[I],.Bank=I,.Stream=I,.Asid=I+1,.Smr=0x80000000|Sids[I],.S2cr=I,
    .Sctlr=0x27,.SctlrIgnoreMask=0x1000,.Cbar=0x1f300,.Cba2r=1,.Tcr=0x803519,.Tcr2=0x38013,
    .Ttbr0=(0xfe000000+I*0x22000)|((UINT64)(I+1)<<48),.Ttbr1=(UINT64)(I+1)<<48,.Mair0=0x44};
  }
  if(First)assert(CrMdssHandoffAppend(&Record,&Mapping)==CR_SUCCESS);
  assert(CrMdssHandoffAppend(&Record,Snapshot)==CR_SUCCESS);
  for(UINT32 Id=2;Id<=16;Id<<=1)if(Id!=CR_MDSS_NATIVE_SMMU){
   CR_MDSS_HANDOFF_PARTICIPANT Empty={.Owner=Id};if(Id==CR_MDSS_NATIVE_DP)Empty.Route.Owner=Id;
   assert(CrMdssHandoffAppend(&Record,&Empty)==CR_SUCCESS);
  }
  if(!First)assert(CrMdssHandoffAppend(&Record,&Mapping)==CR_SUCCESS);
  assert(CrMdssHandoffFinalize(&Record,2)==CR_SUCCESS&&CrMdssHandoffValidate(&Record)==CR_SUCCESS&&
   CrMdssHandoffCanClaim(&Record)==CR_NOT_READY);
 }
}
#ifndef CR_MDP_OUTPUT_FIXTURE_ONLY
int main(VOID){
 VOID *Frame=aligned_alloc(4096,7680*1080);assert(Frame);FramePhysical=(UINTN)Frame;UINT32 Cases=0;
 for(UINT32 Cphy=0;Cphy<2;++Cphy)for(UINT32 Dsc=0;Dsc<2;++Dsc){
  MDP_EXTENDED_PROVIDERS P;MDP_EXTENDED_NATIVE_CONFIG C;CR_MDSS_PANEL_PROFILE F=OutputSetup(&P,&C,Cphy,Dsc);
  memset(Frame,0xa5,7680*1080);
  assert(MdpExtendedOutputOpen(&O,&P,&C,&F,FramePhysical,7680*1080,&Handoff)==EFI_SUCCESS);
  assert(O.Published&&O.Gop.Mode->FrameBufferBase==FramePhysical&&O.Native.FrameIova==0x10800000);
  UINT32 Open=OutputCalls;for(UINTN I=0;I<7680*1080;++I)assert(!((UINT8 *)Frame)[I]);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Red={0,0,0xff,0},Pixels[4];
  assert(O.Gop.Blt(&O.Gop,&Red,EfiBltVideoFill,0,0,2,3,2,2,0)==EFI_SUCCESS);
  assert(O.Gop.Blt(&O.Gop,Pixels,EfiBltVideoToBltBuffer,2,3,0,0,2,2,0)==EFI_SUCCESS);
  for(UINT32 I=0;I<4;++I)assert(!memcmp(Pixels+I,&Red,4));
  assert(O.Gop.Blt(&O.Gop,&Red,EfiBltVideoFill,0,0,MAX_UINTN,0,1,1,0)==EFI_INVALID_PARAMETER);
  assert(O.Gop.Blt(&O.Gop,Pixels,EfiBltBufferToVideo,MAX_UINTN,0,0,0,1,1,4)==EFI_INVALID_PARAMETER);
  UINTN Bytes=0;EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info=NULL;
  assert(O.Gop.QueryMode(&O.Gop,0,&Bytes,&Info)==EFI_SUCCESS&&Info!=&O.Info&&Bytes==sizeof(*Info));FreePool(Info);
  CheckNotifyGop();
  EFI_DISPLAY_POWER_STATE State;
  assert(O.Power.SetDisplayPowerState(&O.Power,EfiDisplayPowerStateOff)==EFI_SUCCESS&&!PanelHardware.Held&&!O.Native.Display.Active&&O.Native.FrameMap);
  assert(O.Power.GetDisplayPowerState(&O.Power,&State)==EFI_SUCCESS&&State==EfiDisplayPowerStateOff);
  assert(O.Power.SetDisplayPowerState(&O.Power,EfiDisplayPowerStateOn)==EFI_SUCCESS&&PanelHardware.Held&&O.Native.Display.Active);
  assert(MdpExtendedOutputClose(&O)==EFI_ACCESS_DENIED);
  EFI_TPL T=Raise(TPL_NOTIFY);assert(OwnerFreeze(&O)==EFI_SUCCESS&&O.Frozen);Restore(T);
  CR_MDSS_HANDOFF_PARTICIPANT Snapshot;VOID *Map=NULL;
  assert(OwnerSnapshot(&O,&Snapshot,&Map)==EFI_SUCCESS&&Snapshot.Route.State==CR_MDSS_NATIVE_UNCERTAIN&&
   (Snapshot.Route.Flags&CR_MDSS_NATIVE_RETAINED_ERROR)&&Map==O.Native.FrameMap);
  ValidateSnapshot(&Snapshot);
  UINT32 Calls=ProviderCalls,Writes=Hardware.Writes;
  assert(O.Gop.SetMode(&O.Gop,0)==EFI_ACCESS_DENIED&&O.Power.SetDisplayPowerState(&O.Power,EfiDisplayPowerStateOn)==EFI_ACCESS_DENIED);
  assert(ProviderCalls==Calls&&Hardware.Writes==Writes&&!Unmaps&&!Frees);RebootFixture();++Cases;
  for(UINT32 Committed=0;Committed<2;++Committed)for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 Fail=1;Fail<=Open;++Fail){
   F=OutputSetup(&P,&C,Cphy,Dsc);OutputFail=Fail;OutputFailure=Positive?1:EFI_DEVICE_ERROR;CommitOutput=Committed;
   EFI_STATUS S=MdpExtendedOutputOpen(&O,&P,&C,&F,FramePhysical,7680*1080,&Handoff);assert(S!=EFI_SUCCESS);
   OutputFail=0;
   if(O.PublicationPending){assert(O.Initialized&&O.Native.Display.Active&&OwnerContext&&ExitCallback&&O.BltConfig);}
   RebootFixture();assert(FrameAttributes==(EFI_MEMORY_WB|EFI_MEMORY_XP));++Cases;
  }
  for(UINT32 Reentrant=0;Reentrant<2;++Reentrant){
   F=OutputSetup(&P,&C,Cphy,Dsc);assert(MdpExtendedOutputOpen(&O,&P,&C,&F,FramePhysical,7680*1080,&Handoff)==EFI_SUCCESS);
   if(Reentrant){FreezeOnFlush=TRUE;assert(O.Gop.Blt(&O.Gop,&Red,EfiBltVideoFill,0,0,0,0,1,2,0)==EFI_ACCESS_DENIED);}
   else{FreezeOnAllocation=TRUE;Info=(VOID *)0x123;Bytes=123;
    assert(O.Gop.QueryMode(&O.Gop,0,&Bytes,&Info)==EFI_ACCESS_DENIED&&Info==(VOID *)0x123&&Bytes==123);}
   T=Raise(TPL_NOTIFY);assert(OwnerFreeze(&O)==EFI_SUCCESS&&O.Frozen);Restore(T);
   assert(!O.Busy&&!Unmaps&&!Frees&&O.Native.FreezeComplete);RebootFixture();++Cases;
  }
  /* A failed publication with no handle remains unpublished, but cleanup
     can fail independently at event drain, cache restore/readback or owner
     retirement. Exact storage survives until that particular retry succeeds. */
  for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 Stage=1;Stage<=4;++Stage){
   F=OutputSetup(&P,&C,Cphy,Dsc);OutputFail=Open;OutputFail2=Open+Stage;
   OutputFailure=Positive?1:EFI_DEVICE_ERROR;
   assert(MdpExtendedOutputOpen(&O,&P,&C,&F,FramePhysical,7680*1080,&Handoff)!=EFI_SUCCESS);
   assert(O.Initialized&&!O.Published&&!O.PublicationPending&&O.Registered&&O.BltConfig&&O.Dxe==&Dxe);
   if(Stage==1)assert(O.ExitEvent&&O.Native.Initialized&&O.Native.FrameMap&&!Unmaps);
   else assert(!O.ExitEvent&&!O.Native.Initialized&&Maps==Unmaps);
   OutputFail=OutputFail2=0;RebootFixture();++Cases;
  }
  F=OutputSetup(&P,&C,Cphy,Dsc);NullPublication=TRUE;
  assert(MdpExtendedOutputOpen(&O,&P,&C,&F,FramePhysical,7680*1080,&Handoff)==EFI_DEVICE_ERROR&&
   O.PublicationPending&&O.Initialized&&O.Native.Display.Active);
  RebootFixture();++Cases;
 }
 /* Controller1 retains its actual INTF2/DSI1/PHY1 identity in diagnostics;
    neither append ordering nor revoked evidence can turn it into a claim. */
 MDP_EXTENDED_PROVIDERS P;MDP_EXTENDED_NATIVE_CONFIG C;
 CR_MDSS_PANEL_PROFILE F=OutputSetup(&P,&C,FALSE,FALSE);
 CR_MDSS_PANEL_COMMAND SingleOn[ARRAY_SIZE(On)],SingleOff[ARRAY_SIZE(Off)];
 memcpy(SingleOn,On,sizeof(On));memcpy(SingleOff,Off,sizeof(Off));
 for(UINT32 I=0;I<ARRAY_SIZE(SingleOn);++I)if(SingleOn[I].DataType!=0xff)SingleOn[I].ControllerMask=2;
 for(UINT32 I=0;I<ARRAY_SIZE(SingleOff);++I)SingleOff[I].ControllerMask=2;
 F.On=SingleOn;F.Off=SingleOff;F.Mode.Controllers=1;F.Mode.FirstController=1;F.Mode.Raster.Controller=1;
 assert(MdpExtendedOutputOpen(&O,&P,&C,&F,FramePhysical,7680*1080,&Handoff)==EFI_SUCCESS);
 EFI_TPL T=Raise(TPL_NOTIFY);assert(OwnerFreeze(&O)==EFI_SUCCESS);Restore(T);
 CR_MDSS_HANDOFF_PARTICIPANT Snapshot;VOID *Map=NULL;
 assert(OwnerSnapshot(&O,&Snapshot,&Map)==EFI_SUCCESS&&Snapshot.Route.Controller==1&&
  Snapshot.Route.InterfaceBase==0xae37000&&Snapshot.Route.LinkBase==0xae96000);
 ValidateSnapshot(&Snapshot);RebootFixture();++Cases;
 free(Frame);
 printf("Actual native extended GOP/core/clock owner: pixels, power cycles, EBS, %u publication/lifetime cases PASS\n",Cases);return 0;
}
#endif
