/* Actual common GOP with an asynchronous pipeline boundary. GPL-2.0-only */
#define CR_MDP_OUTPUT_FIXTURE_ONLY
#include "MdpExtendedOutputTest.c"
static EFI_STATUS PipelineStatus,FreezeResult;
static UINT32 PipelinePublishes,PipelineFreezes;
static UINTN PublishedOffset,PublishedBytes;
static UINT32 PipelineIdentity;
static EFI_SMMU_CR_EXIT_PREPARE PrepareCallback;
static VOID *PrepareContext;
static EFI_STATUS PipelineReady(VOID *C){assert(C==&PipelineIdentity);return PipelineStatus;}
static EFI_STATUS PipelinePublish(VOID *C,UINTN Offset,UINTN Bytes){
 assert(C==&PipelineIdentity&&Bytes&&Offset+Bytes<=O.Mode.FrameBufferSize);
 ++PipelinePublishes;PublishedOffset=Offset;PublishedBytes=Bytes;return PipelineStatus;
}
static EFI_STATUS EFIAPI PipelineFreeze(VOID *C){
 assert(C==&PipelineIdentity&&CurrentTpl==TPL_NOTIFY);++PipelineFreezes;return FreezeResult;
}
static EFI_STATUS PipelineClose(VOID *C){assert(C==&PipelineIdentity);return EFI_SUCCESS;}
static EFI_STATUS PipelineSetPower(VOID *C,EFI_DISPLAY_POWER_STATE S){assert(C==&PipelineIdentity);return S==EfiDisplayPowerStateOn?PipelineStatus:EFI_UNSUPPORTED;}
static EFI_STATUS PipelineGetPower(VOID *C,EFI_DISPLAY_POWER_STATE *S){assert(C==&PipelineIdentity);*S=EfiDisplayPowerStateOn;return PipelineStatus;}
static CONST MDP_OUTPUT_PIPELINE Pipeline={PipelineReady,PipelinePublish,PipelineFreeze,PipelineClose,PipelineSetPower,PipelineGetPower};
static EFI_STATUS EFIAPI PipelinePrepare(EFI_SMMU_CR_PROTOCOL *P,VOID *Owner,EFI_SMMU_CR_EXIT_PREPARE F,VOID *C){
 assert(P==&Smmu&&Owner==&O);EFI_STATUS S=OutputStep();if(S!=EFI_SUCCESS)return S;
 if(F){assert(C==&O&&!PrepareCallback);PrepareCallback=F;PrepareContext=C;}
 else{assert(!C);PrepareCallback=NULL;PrepareContext=NULL;}return S;
}
static EFI_STATUS EFIAPI PipelineCreate(UINT32 Type,EFI_TPL T,EFI_EVENT_NOTIFY F,CONST VOID *C,CONST EFI_GUID *G,EFI_EVENT *E){
 assert(C==&O&&!OwnerContext);OwnerContext=(VOID *)C;
 EFI_STATUS S=Create(Type,T,F,C,G,E);OwnerContext=NULL;return S;
}
static EFI_STATUS EFIAPI PipelineInstall(EFI_HANDLE *Handle,...){
 assert(PipelineStatus==EFI_SUCCESS&&O.CacheReady&&O.BltConfig&&!O.Exiting);++OutputInstalls;
 UINT32 Count=0;EFI_GUID *G;VOID *P;va_list A;va_start(A,Handle);
 while((G=va_arg(A,EFI_GUID *))){P=va_arg(A,VOID *);++Count;
  if(G==&gEfiDevicePathProtocolGuid)assert(P==&O.Path);
  else if(G==&gEfiGraphicsOutputProtocolGuid)assert(P==&O.Gop);
  else if(G==&gEfiDisplayPowerStateProtocolGuid)assert(P==&O.Power);else assert(0);
 }va_end(A);assert(Count==3);EFI_STATUS S=OutputStep();
 if((S==EFI_SUCCESS||CommitOutput)&&!NullPublication)*Handle=(VOID *)0x222;return S;
}
static VOID PipelineSetup(VOID){
 MDP_EXTENDED_PROVIDERS P;MDP_EXTENDED_NATIVE_CONFIG C;(VOID)OutputSetup(&P,&C,FALSE,FALSE);
 PipelineStatus=EFI_NOT_READY;FreezeResult=EFI_SUCCESS;PipelinePublishes=PipelineFreezes=0;
 PrepareCallback=NULL;PrepareContext=NULL;FrameAttributes=EFI_MEMORY_WC|EFI_MEMORY_XP;
 Boot.CreateEventEx=PipelineCreate;Boot.InstallMultipleProtocolInterfaces=PipelineInstall;
 Smmu.SetExitPrepareObserver=PipelinePrepare;
}
static EFI_STATUS PipelineOpen(VOID){
 EFI_STATUS S=MdpExtendedOutputInitPipeline(&O,&Dxe,&Pipeline,&PipelineIdentity,FramePhysical,1920,1080,7680);
 if(S==EFI_SUCCESS)S=MdpExtendedOutputBindPrepare(&O,&Smmu);
 if(S==EFI_SUCCESS)S=MdpExtendedOutputConfigureFramebuffer(&O);
 return S;
}
static VOID PipelineReset(VOID){
 O.Published=O.PublicationPending=O.Exiting=O.Frozen=FALSE;
 OutputFail=OutputFail2=0;assert(MdpExtendedOutputClose(&O)==EFI_SUCCESS);
 assert(!PoolCount&&!ExitCallback&&!PrepareCallback&&!O.Initialized);NativeClockFixtureFinish();
}
int main(void){
 VOID *Frame=aligned_alloc(4096,8294400);assert(Frame);FramePhysical=(UINTN)Frame;
 PipelineSetup();memset(Frame,0x55,8294400);assert(PipelineOpen()==EFI_SUCCESS);
 assert(!OutputInstalls&&!O.Published&&PrepareCallback&&PrepareContext==&O);
 assert(MdpExtendedOutputPublishPipeline(&O)==EFI_NOT_READY&&!OutputInstalls);
 /* A real producer completion changes this boundary; polling/time does not. */
 PipelineStatus=EFI_SUCCESS;assert(MdpExtendedOutputPublishPipeline(&O)==EFI_SUCCESS&&O.Published&&OutputInstalls==1);
 assert(MdpExtendedOutputPublishPipeline(&O)==EFI_SUCCESS&&OutputInstalls==1);
 EFI_GRAPHICS_OUTPUT_BLT_PIXEL Color={.Blue=5,.Green=7,.Red=9};
 assert(O.Gop.Blt(&O.Gop,&Color,EfiBltVideoFill,0,0,2,3,4,2,0)==EFI_SUCCESS);
 assert(PipelinePublishes==1&&PublishedOffset==3*7680+8&&PublishedBytes==7680+16);
 assert(((EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)Frame)[3*1920+2].Red==9);
 CheckNotifyGop();
 /* Restore the witness pixel cleared by the notify-level SetMode test. */
 assert(O.Gop.Blt(&O.Gop,&Color,EfiBltVideoFill,0,0,2,3,4,2,0)==EFI_SUCCESS);
 PipelineStatus=EFI_DEVICE_ERROR;
 assert(O.Gop.SetMode(&O.Gop,0)==EFI_DEVICE_ERROR&&((EFI_GRAPHICS_OUTPUT_BLT_PIXEL*)Frame)[3*1920+2].Red==9);
 PipelineStatus=EFI_SUCCESS;EFI_TPL Old=Raise(TPL_NOTIFY);FreezeResult=EFI_TIMEOUT;
 assert(PrepareCallback(PrepareContext)==EFI_TIMEOUT&&O.Exiting&&O.Frozen&&!O.PipelineFrozen);
 assert(PipelineFreezes==1&&!Unmaps&&!Frees&&PrepareCallback);
 FreezeResult=EFI_SUCCESS;assert(PrepareCallback(PrepareContext)==EFI_SUCCESS&&O.PipelineFrozen&&PipelineFreezes==2);
 assert(PrepareCallback(PrepareContext)==EFI_SUCCESS&&PipelineFreezes==2);Restore(Old);
 assert(O.Gop.SetMode(&O.Gop,0)==EFI_ACCESS_DENIED&&MdpExtendedOutputClose(&O)==EFI_ACCESS_DENIED);
 PipelineReset();UINT32 Cases=1;
 PipelineSetup();assert(PipelineOpen()==EFI_SUCCESS);PipelineStatus=EFI_SUCCESS;
 assert(MdpExtendedOutputPublishPipeline(&O)==EFI_SUCCESS);UINT32 Count=OutputCalls;PipelineReset();
 for(UINT32 Warning=0;Warning<2;++Warning)for(UINT32 Commit=0;Commit<2;++Commit)for(UINT32 F=1;F<=Count;++F){
  PipelineSetup();OutputFail=F;OutputFailure=Warning?EFI_WARN_UNKNOWN_GLYPH:EFI_DEVICE_ERROR;CommitOutput=Commit;
  EFI_STATUS S=PipelineOpen();PipelineStatus=EFI_SUCCESS;if(S==EFI_SUCCESS)S=MdpExtendedOutputPublishPipeline(&O);
  assert(S!=EFI_SUCCESS&&!O.Published);PipelineReset();++Cases;
 }
 PipelineSetup();assert(PipelineOpen()==EFI_SUCCESS);PipelineStatus=EFI_SUCCESS;NullPublication=TRUE;
 assert(MdpExtendedOutputPublishPipeline(&O)==EFI_DEVICE_ERROR&&O.PublicationPending&&!O.Published);
 assert(MdpExtendedOutputClose(&O)==EFI_ACCESS_DENIED);PipelineReset();++Cases;
 free(Frame);printf("Common actual GOP pipeline PASS: %u asynchronous publication/cache/BLT/retained EBS cases\n",Cases);return 0;
}
