/* Actual HDK XML -> producer -> entry -> GOP -> native/private-clock/cores.
 * Only physical/boot-service boundary effects are modeled. GPL-2.0-only */
#define CR_MDP_OUTPUT_FIXTURE_ONLY
#define MicroSecondDelay CoreMicroSecondDelay
#include "MdpExtendedOutputTest.c"
#undef MicroSecondDelay
#include "../Driver/MDSSCrDxe/MdpExtendedEntry.h"
#include "../Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.h"
#include <Library/CrDalLib.h>
#include <Library/DxeServicesLib.h>
#include <Library/MemoryMapHelperLib.h>
/* Compile the complete producer, with its private names isolated from the
   reused actual output fixture. This exposes no production test API. */
#define Exact ProfileExact
#define Enter ProfileEnter
#define Leave ProfileLeave
#define Bridge ProfileBridge
#define PanelReady ProfilePanelReady
#define PanelAcquire ProfilePanelAcquire
#define PanelRelease ProfilePanelRelease
#define PanelReset ProfilePanelReset
#define PanelPrepare ProfilePanelPrepare
#define Configure ProfileConfigure
#define GetFramebuffer ProfileFramebuffer
#define BridgeReady ProfileBridgeReady
#define Exit ProfileExit
#include "../Driver/MDSSCrDxe/MdpHdkExtendedProfile.c"
#undef Exact
#undef Enter
#undef Leave
#undef Bridge
#undef PanelReady
#undef PanelAcquire
#undef PanelRelease
#undef PanelReset
#undef PanelPrepare
#undef Configure
#undef GetFramebuffer
#undef BridgeReady
#undef Exit
EFI_DXE_SERVICES *gDS=&Dxe;
EFI_GUID gEfiMdssPanelProfileCrProtocolGuid,gEfiLt9611PanelCrProtocolGuid,gEfiMdpDsiProtocolGuid;
EFI_GUID gEfiClockCrProtocolGuid,gEfiClockExternalCrProtocolGuid,gEfiRpmhCrProtocolGuid;
EFI_GUID gEfiInterconnectCrProtocolGuid,gEfiSmmuCrProtocolGuid,gEdkiiIoMmuProtocolGuid,gEfiMdssHandoffProtocolGuid;
static MDP_EXTENDED_PROVIDERS ActualProviders;
static LT9611_PANEL_OWNER BridgeOwner;
static UINT8 Levels[110];
static UINT16 Pins[3]={9,109,107};
static CR_DAL_LT9611_CONFIG Target={.I2cInstance=10,.I2cAddress=0x2b,.InterruptPin=44,.PowerPinCount=3,.PowerPins=Pins};
static CR_MDSS_TARGET SelectedMdss;
static UINT32 Calls,Fail,CloseFail,VideoPrepares,PinWrites,TestPhase;
static EFI_STATUS Failure;
static BOOLEAN Commit,Live,McuBusy,MissingBridge,FreezeInsideInstall;
static EFI_EVENT_NOTIFY Callbacks[2];
static VOID *Contexts[2];
static VOID *XmlBytes;
static UINTN XmlSize;
static EFI_MDSS_PANEL_PROFILE_CR_PROTOCOL *Selected;
static EFI_MDP_DSI_PROTOCOL *Prepared;
static UINT32 ProducerInstalls,GopInstalls,UninstallCount;
static EFI_STATUS BoundaryStep(VOID){++Calls;return Calls==Fail||Calls==CloseFail?Failure:EFI_SUCCESS;}
UINTN EFIAPI MicroSecondDelay(UINTN Us){assert(CurrentTpl<=TPL_CALLBACK||Us<=20000);
  assert(Us<=300000);return Us;}
UINTN EFIAPI AsciiStrLen(CONST CHAR8 *S){return strlen(S);}
VOID *EFIAPI AllocateZeroPool(UINTN N){VOID *P=AllocatePool(N);if(P)memset(P,0,N);return P;}
CONST CR_DAL_LT9611_CONFIG *CrDalGetLt9611Config(VOID){return &Target;}
CONST CR_MDSS_TARGET *CrDalGetMdssContext(VOID){return SelectedMdss.Soc?&SelectedMdss:CrTargetGetMdssContext();}
EFI_STATUS EFIAPI GetSectionFromAnyFv(CONST EFI_GUID *G,UINT8 Type,UINTN Instance,VOID **Buffer,UINTN *Size){
  assert(G->Data1==0x7321f393&&Type==EFI_SECTION_RAW&&!Instance);EFI_STATUS S=BoundaryStep();
  if(S==EFI_SUCCESS||Commit){*Buffer=AllocateCopyPool(XmlSize,XmlBytes);assert(*Buffer);*Size=XmlSize;}return S;
}
EFI_STATUS EFIAPI LocateMemoryMapAreaByName(CHAR8 *Name,ARM_MEMORY_REGION_DESCRIPTOR_EX *R){
  assert(!strcmp(Name,"Display Reserved"));EFI_STATUS S=BoundaryStep();if(S==EFI_SUCCESS){memset(R,0,sizeof(*R));R->Address=FramePhysical;R->Length=0x2b00000;}return S;
}
static EFI_STATUS IoReady(VOID *C){assert(C==&BridgeOwner);if(!Live)return EFI_ACCESS_DENIED;return BoundaryStep();}
static BOOLEAN IoLock(VOID *C){assert(C==&BridgeOwner&&CurrentTpl<=TPL_CALLBACK);if(McuBusy)return FALSE;McuBusy=TRUE;return TRUE;}
static VOID IoUnlock(VOID *C){assert(C==&BridgeOwner&&McuBusy);McuBusy=FALSE;}
static EFI_STATUS IoPrepare(VOID *C,CONST EFI_MDP_DSI_TIMING *T){
  assert(C==&BridgeOwner&&McuBusy&&BridgeOwner.Powered&&Levels[9]&&Levels[109]&&Levels[107]);
  assert(T->Width==1920&&T->Height==1080&&T->DsiLanes==4&&T->DsiController==0);
  assert(CacheFlushes&&!(Hardware.R[CrMdssExtDsi0][1]&BIT1));
  ++VideoPrepares;return BoundaryStep();
}
static EFI_STATUS EFIAPI PinSet(EFI_GPIO_CR_PROTOCOL *G,UINT16 Pin,UINT8 Value){
  assert(G&&McuBusy&&(Pin==9||Pin==109||Pin==107)&&Value<=1);++PinWrites;EFI_STATUS S=BoundaryStep();
  if(S==EFI_SUCCESS||Commit)Levels[Pin]=Value;return S;
}
static EFI_STATUS EFIAPI PinRead(EFI_GPIO_CR_PROTOCOL *G,UINT16 Pin,UINT8 *Value){
  assert(G&&(Pin==9||Pin==109||Pin==107)&&Value);EFI_STATUS S=BoundaryStep();if(S==EFI_SUCCESS)*Value=Levels[Pin];return S;
}
static EFI_GPIO_CR_PROTOCOL Gpio={.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.SetIoValue=PinSet,.ReadIoValue=PinRead};
static EFI_STATUS EFIAPI LocateActual(EFI_GUID *G,VOID *Registration,VOID **Out){
  assert(!Registration);EFI_STATUS S=BoundaryStep();if(S!=EFI_SUCCESS)return S;
  if(G==&gEfiLt9611PanelCrProtocolGuid){if(MissingBridge)return EFI_NOT_FOUND;*Out=&BridgeOwner.Protocol;}
  else if(G==&gEfiClockCrProtocolGuid)*Out=ActualProviders.Clock;
  else if(G==&gEfiClockExternalCrProtocolGuid)*Out=ActualProviders.External;
  else if(G==&gEfiRpmhCrProtocolGuid)*Out=ActualProviders.Rpmh;
  else if(G==&gEfiInterconnectCrProtocolGuid)*Out=ActualProviders.Icb;
  else if(G==&gEfiSmmuCrProtocolGuid)*Out=ActualProviders.Smmu;
  else if(G==&gEdkiiIoMmuProtocolGuid)*Out=ActualProviders.Iommu;
  else if(G==&gEfiMdssHandoffProtocolGuid)*Out=&Handoff;
  else assert(0);return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ActualOwner(EFI_MDSS_HANDOFF_PROTOCOL *H,UINT32 Id,VOID *C,
  EFI_MDSS_HANDOFF_FREEZE F,EFI_MDSS_HANDOFF_SNAPSHOT S){
  (void)H;assert(Id==CR_MDSS_NATIVE_DSI&&C==&mProfile.Entry.Output);EFI_STATUS E=BoundaryStep();if(E!=EFI_SUCCESS)return E;
  if(F){assert(!OwnerContext&&S);OwnerContext=C;OwnerFreeze=F;OwnerSnapshot=S;}
  else{assert(OwnerContext==C);OwnerContext=NULL;OwnerFreeze=NULL;OwnerSnapshot=NULL;}return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI ActualCreate(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY F,CONST VOID *C,
  CONST EFI_GUID *G,EFI_EVENT *E){
  assert(Type==EVT_NOTIFY_SIGNAL&&T==TPL_NOTIFY&&G==&gEfiEventExitBootServicesGuid&&F);
  UINT32 I=C==&mProfile?0:1;assert((I==0)||C==OwnerContext);EFI_STATUS S=BoundaryStep();
  if(S==EFI_SUCCESS||Commit){*E=(VOID *)(UINTN)(I+1);Callbacks[I]=F;Contexts[I]=(VOID *)C;}return S;
}
static EFI_STATUS EFIAPI ActualClose(EFI_EVENT E){UINT32 I=(UINT32)(UINTN)E-1;assert(I<2&&Callbacks[I]);
  EFI_STATUS S=BoundaryStep();if(S==EFI_SUCCESS){Callbacks[I]=NULL;Contexts[I]=NULL;}return S;
}
static EFI_STATUS EFIAPI ActualInstall(EFI_HANDLE *Handle,...){
  EFI_GUID *G;VOID *P;UINT32 Count=0;BOOLEAN Profile=FALSE;va_list A;va_start(A,Handle);
  while((G=va_arg(A,EFI_GUID *))){P=va_arg(A,VOID *);++Count;
    if(G==&gEfiMdssPanelProfileCrProtocolGuid){assert(P==&mProfile.Selection);Selected=P;Profile=TRUE;}
    else if(G==&gEfiMdpDsiProtocolGuid){assert(P==&mProfile.Prepared);Prepared=P;}
    else if(G==&gEfiGraphicsOutputProtocolGuid){assert(P==&mProfile.Entry.Output.Gop&&mProfile.Entry.Output.Native.Display.Active);}
    else if(G==&gEfiDevicePathProtocolGuid)assert(P==&mProfile.Entry.Output.Path);
    else if(G==&gEfiDisplayPowerStateProtocolGuid)assert(P==&mProfile.Entry.Output.Power);
    else assert(0);
  }va_end(A);assert(Count==(Profile?2U:3U));if(Profile)++ProducerInstalls;else ++GopInstalls;
  EFI_STATUS S=BoundaryStep();if(S==EFI_SUCCESS||Commit)*Handle=(VOID *)(UINTN)(Profile?0x333:0x222);
  if(FreezeInsideInstall){FreezeInsideInstall=FALSE;EFI_TPL T=Raise(TPL_NOTIFY);Callbacks[0]((VOID *)1,Contexts[0]);Restore(T);}
  return S;
}
static EFI_STATUS EFIAPI ActualUninstall(EFI_HANDLE Handle,...){assert(Handle==(VOID *)0x333);++UninstallCount;
  EFI_STATUS S=BoundaryStep();if(S==EFI_SUCCESS){Selected=NULL;Prepared=NULL;}return S;
}
static EFI_STATUS EFIAPI ActualAttributes(UINT64 Base,UINT64 Bytes,UINT64 Attr){
  assert(Base==FramePhysical&&Bytes==8294400&&!mProfile.Entry.Output.Native.Display.Active);
  EFI_STATUS S=OutputStep();if(S==EFI_SUCCESS||CommitOutput)FrameAttributes=Attr;return S;
}
static VOID BeginImage(VOID){
  assert(!MdpHdkExtendedProfileRetained()&&!PoolCount&&!CommandAllocation&&!OwnerContext&&!Callbacks[0]&&!Callbacks[1]);
  MDP_EXTENDED_NATIVE_CONFIG C;(VOID)OutputSetup(&ActualProviders,&C,FALSE,FALSE);
  Calls=Fail=CloseFail=VideoPrepares=PinWrites=ProducerInstalls=GopInstalls=UninstallCount=0;
  Failure=EFI_DEVICE_ERROR;Commit=FALSE;Live=TRUE;McuBusy=MissingBridge=FreezeInsideInstall=FALSE;
  memset(&BridgeOwner,0,sizeof(BridgeOwner));memset(Levels,0,sizeof(Levels));Levels[9]=Levels[109]=Levels[107]=1;
  LT9611_PANEL_OWNER_IO Io={&BridgeOwner,IoReady,IoLock,IoUnlock,IoPrepare,NULL};
  assert(Lt9611PanelOwnerInit(&BridgeOwner,&Gpio,&Io)==EFI_SUCCESS);Calls=0;
  Boot=(EFI_BOOT_SERVICES){.RaiseTPL=Raise,.RestoreTPL=Restore,.LocateProtocol=LocateActual,
    .CreateEventEx=ActualCreate,.CloseEvent=ActualClose,.InstallMultipleProtocolInterfaces=ActualInstall,
    .UninstallMultipleProtocolInterfaces=ActualUninstall};
  Dxe.SetMemorySpaceAttributes=ActualAttributes;Handoff.SetParticipant=ActualOwner;
}
static VOID ResetImage(VOID){
  /* Simulated reboot only. Production refuses unloading a published GOP and
     never reopens these gates after EBS. Then exercise actual ordered cleanup. */
  mProfile.Frozen=FALSE;mProfile.PublicationPending=FALSE;
  MDP_EXTENDED_OUTPUT *Q=&mProfile.Entry.Output;
  Q->Published=Q->PublicationPending=Q->Frozen=Q->Exiting=FALSE;Q->Native.Frozen=FALSE;
  Fail=CloseFail=0;OutputFail=OutputFail2=0;Live=TRUE;
  EFI_STATUS Retire=MdpHdkExtendedProfileUnload();
  if(Retire!=EFI_SUCCESS)fprintf(stderr,"reset phase=%u retire=%llx entry=%u out=%u panelowned=%u source=%u native=%u close=%u reg=%u events=%u/%u frozen=%u/%u\n",TestPhase,(unsigned long long)Retire,mProfile.Entry.Initialized,Q->Initialized,mProfile.PanelOwned,mProfile.Entry.SourceOwned,Q->Native.Initialized,Q->Native.Closing,Q->Registered,Callbacks[0]!=NULL,Callbacks[1]!=NULL,Q->Native.Frozen,mProfile.Frozen);
  if(Retire!=EFI_SUCCESS)fprintf(stderr,"panel power%u reset%u prep%u pending%u; disp panel%u pending%u dsi pending%u ext%u clocks%u%u%u%u%u%u%u\n",Q->Native.Panel.PowerOwned,Q->Native.Panel.ResetTouched,Q->Native.Panel.Prepared,Q->Native.Panel.CleanupPending,Q->Native.Display.PanelOwned,Q->Native.Display.CleanupPending,Q->Native.Display.Dsi.CleanupPending,Q->Native.ExternalOwned,Q->Native.ClockOwned[0],Q->Native.ClockOwned[1],Q->Native.ClockOwned[2],Q->Native.ClockOwned[3],Q->Native.ClockOwned[4],Q->Native.ClockOwned[5],Q->Native.ClockOwned[6]);
  assert(Retire==EFI_SUCCESS&&!MdpHdkExtendedProfileRetained());
  assert(!PoolCount&&!OwnerContext&&!Callbacks[0]&&!Callbacks[1]&&!CommandAllocation&&Maps==Unmaps);
  assert(!BridgeOwner.Owner&&!McuBusy&&!NativeClockFixtureOwned());NativeClockFixtureFinish();
}
int main(int argc,char **argv){
  assert(argc==2);FILE *F=fopen(argv[1],"rb");assert(F);assert(!fseek(F,0,SEEK_END));long N=ftell(F);assert(N>0&&!fseek(F,0,SEEK_SET));
  XmlBytes=malloc(N);assert(XmlBytes&&fread(XmlBytes,1,N,F)==(UINTN)N&&!fclose(F));XmlSize=N;
  VOID *Frame=aligned_alloc(4096,8294400);assert(Frame);FramePhysical=(UINTN)Frame;UINT32 Cases=0;
  BeginImage();assert(MdpHdkExtendedProfileStart()==EFI_SUCCESS&&Selected&&Prepared&&ProducerInstalls==1&&!GopInstalls);
  assert(Prepared->BridgeReady(Prepared)==EFI_SUCCESS&&GopInstalls==1&&VideoPrepares==1&&BridgeOwner.Owner);
  UINT32 OpenCalls=Calls;MDP_EXTENDED_OUTPUT *Q=&mProfile.Entry.Output;
  UINT32 WritesBefore=Hardware.Writes,MapBefore=Maps;
  assert(Prepared->BridgeReady(Prepared)==EFI_SUCCESS&&GopInstalls==1&&VideoPrepares==1&&Hardware.Writes==WritesBefore&&Maps==MapBefore);
  assert(Q->Published&&Q->Mode.FrameBufferBase==FramePhysical&&Q->Mode.Info->PixelFormat==PixelBlueGreenRedReserved8BitPerColor);
  for(UINTN I=0;I<8294400;++I)assert(!((UINT8 *)Frame)[I]);
  EFI_PHYSICAL_ADDRESS Base;UINT32 W,H,Stride;
  assert(Prepared->GetFramebuffer(Prepared,&Base,&W,&H,&Stride)==EFI_SUCCESS&&Base==FramePhysical&&W==1920&&H==1080&&Stride==7680);
  assert(MdpHdkExtendedProfileUnload()==EFI_ACCESS_DENIED&&mProfile.Published&&Q->Published);
  assert(Q->Power.SetDisplayPowerState(&Q->Power,EfiDisplayPowerStateOff)==EFI_SUCCESS&&!BridgeOwner.Owner&&!BridgeOwner.Powered&&!Q->Native.Display.Active);
  assert(Q->Power.SetDisplayPowerState(&Q->Power,EfiDisplayPowerStateOn)==EFI_SUCCESS&&BridgeOwner.Owner&&BridgeOwner.Powered&&VideoPrepares==2);
  UINT32 PinBefore=PinWrites,MapsBefore=Maps;EFI_TPL T=Raise(TPL_NOTIFY);
  Callbacks[0]((VOID *)1,Contexts[0]);assert(Q->Frozen&&Q->Native.FreezeComplete);
  Callbacks[1]((VOID *)2,Contexts[1]);Restore(T);
  assert(!Unmaps&&!Frees&&Maps==MapsBefore&&PinWrites==PinBefore&&BridgeOwner.Owner);
  assert(Prepared->BridgeReady(Prepared)==EFI_ACCESS_DENIED&&Q->Gop.SetMode(&Q->Gop,0)==EFI_ACCESS_DENIED);
  CR_MDSS_HANDOFF_PARTICIPANT Snapshot;VOID *Mapping;
  assert(OwnerSnapshot(OwnerContext,&Snapshot,&Mapping)==EFI_SUCCESS&&Snapshot.Route.State==CR_MDSS_NATIVE_UNCERTAIN);
  ValidateSnapshot(&Snapshot);ResetImage();++Cases;
  for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 Committed=0;Committed<2;++Committed)for(UINT32 Phase=1;Phase<=OpenCalls;++Phase){
    BeginImage();TestPhase=Phase;Fail=Phase;Failure=Positive?1:EFI_DEVICE_ERROR;Commit=Committed;
    EFI_STATUS S=MdpHdkExtendedProfileStart();if(S==EFI_SUCCESS)S=Prepared->BridgeReady(Prepared);
    if(S==EFI_SUCCESS)fprintf(stderr,"unexercised producer phase%u calls%u\n",Phase,Calls);
    assert(S!=EFI_SUCCESS&&!McuBusy);Fail=0;ResetImage();++Cases;
  }
  /* Unpublished source callbacks and owned XML must remain resident until
     each actual uninstall/event drain returns exact completed success. */
  for(UINT32 Positive=0;Positive<2;++Positive)for(UINT32 Stage=1;Stage<=2;++Stage){
    BeginImage();assert(MdpHdkExtendedProfileStart()==EFI_SUCCESS);
    Fail=Calls+Stage;Failure=Positive?1:EFI_DEVICE_ERROR;
    assert(MdpHdkExtendedProfileUnload()!=EFI_SUCCESS&&MdpHdkExtendedProfileRetained()&&Callbacks[0]);
    assert(mProfile.Published==(Stage==1)&&mProfile.Xml.InitSequence.Count==0&&!Maps);
    Fail=0;ResetImage();++Cases;
  }
  BeginImage();Pins[0]=10;
  assert(MdpHdkExtendedProfileStart()==EFI_UNSUPPORTED&&!MdpHdkExtendedProfileRetained()&&!Calls);
  Pins[0]=9;ResetImage();++Cases;
  BeginImage();MissingBridge=TRUE;assert(MdpHdkExtendedProfileStart()==EFI_SUCCESS);
  assert(Prepared->BridgeReady(Prepared)==EFI_NOT_FOUND&&!mProfile.Entry.Initialized&&!OwnerContext&&!NativeClockFixtureOwned());
  MissingBridge=FALSE;assert(Prepared->BridgeReady(Prepared)==EFI_SUCCESS);ResetImage();++Cases;
  BeginImage();FreezeInsideInstall=TRUE;
  assert(MdpHdkExtendedProfileStart()==EFI_SUCCESS&&mProfile.Frozen&&!mProfile.Entry.Initialized);
  assert(Prepared->BridgeReady(Prepared)==EFI_ACCESS_DENIED&&!Maps&&!NativeClockFixtureOwned());ResetImage();++Cases;
  BeginImage();SelectedMdss=*CrTargetGetMdssContext();SelectedMdss.Soc=CR_MDSS_SOC_SM8350;
  Target.I2cInstance=15;Target.InterruptPin=50;Pins[0]=47;Pins[1]=49;Pins[2]=48;
  assert(MdpHdkExtendedProfileStart()==EFI_SUCCESS&&Selected&&Prepared&&!GopInstalls);
  assert(mProfile.Xml.Timing.DisplayResetGpio==48&&mProfile.Panel.Profile.Mode.Raster.Soc==CR_MDSS_SOC_SM8350);
  ResetImage();++Cases;
  free(Frame);free(XmlBytes);
  printf("Actual HDK producer/entry/GOP/native/clock/core: %u exact failure/power/EBS/deferred profile cases PASS (%u acquisition steps)\n",Cases,OpenCalls);return 0;
}
