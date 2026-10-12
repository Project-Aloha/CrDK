/* Actual command adapter/core/panel/events/OSKAL composition. GPL-2.0-only
 * Common resource methods are the boundary model; their real typed SMMU
 * composition is separately exercised by MdpNativeResourcesTest.c. */
#define MDSS_TEST_COMMAND_BYTES 4096
#define main EarlierLifecycleMain
#include "MdssCmdLifecycleTest.c"
#undef main
#include "MdpNativeEventsFixture.h"
#include "../Driver/MDSSCrDxe/MdpCommandNative.h"
static MDP_COMMAND_NATIVE N;
static CR_MDSS_TARGET Target;
static EFI_GPIO_CR_PROTOCOL Gpio;
static EFI_RPMH_CR_PROTOCOL NativeRpmh;
static EFI_SMMU_CR_PROTOCOL NativeSmmu;
static EFI_SMMU_CR_EXIT_PREPARE NativePrepare;
static UINT32 Notifications,Checks,Publishes;
static EFI_STATUS NotificationStatus;
static BOOLEAN MappingLost,ResourceClosed;
static UINT8 ResetLevel;
static CR_MDSS_CMD_WINDOW NativeWindow(CR_MDSS_RETIRE_WINDOW W){
 if(W==CrMdssRetireDpu)return CrMdssCmdDpu;
 if(W==CrMdssRetireDsi0)return CrMdssCmdHost;
 if(W==CrMdssRetirePhy0)return CrMdssCmdPhy;
 assert(W==CrMdssRetireVbif);return CrMdssCmdVbif;
}
EFI_STATUS MdpNativeResourcesOpen(MDP_NATIVE_RESOURCES *R,CONST MDP_NATIVE_PROVIDERS *P,
 CONST MDP_NATIVE_RESOURCE_CONFIG *C,UINT32 Controllers,EFI_PHYSICAL_ADDRESS A,UINTN Bytes){
 assert(R==&N.Resources&&C->Target==&Target&&Controllers==1&&A==0x82000000&&Bytes==sizeof(Frame));
 if(!R->Initialized){R->Initialized=TRUE;R->Providers=*P;R->Config=*C;R->OwnerGeneration=91;
  R->Hardware=CrMdssGetHardwareProfile(C->Target->Soc);R->FrameCpu=Frame;R->FrameBytes=Bytes;
  R->CommandCpu=Commands;R->FrameIova=0x82000000;R->CommandIova=0x83000000;R->Stage=2;}
 assert(C->BlankOutput&&C->OutputContext==&N);
 EFI_STATUS S=C->BlankOutput(C->OutputContext);if(S!=EFI_SUCCESS)return S;
 assert(N.Panel.PinOwned[0]&&!ResetLevel&&"reset must precede inherited MDSS disruption");
 R->OutputBlanked=TRUE;R->AccessReady=TRUE;
 return CR_SUCCESS;
}
EFI_STATUS MdpNativeResourcesSuppliersReady(MDP_NATIVE_RESOURCES *R){
 assert(R==&N.Resources&&R->Initialized&&R->Stage>=2);return Step(&H);
}
UINTN EFIAPI MicroSecondDelay(UINTN Us){assert(Us&&Us<=20000);return Us;}
EFI_STATUS MdpNativeResourcesReady(MDP_NATIVE_RESOURCES *R){
 assert(R==&N.Resources);if(MappingLost||ResourceClosed)return EFI_DEVICE_ERROR;return Step(&H);
}
EFI_STATUS MdpNativeResourcesCheck(MDP_NATIVE_RESOURCES *R){++Checks;return MdpNativeResourcesReady(R);}
EFI_STATUS MdpNativeResourcesRead(MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 *V){
 assert(R==&N.Resources);return Read(&H,NativeWindow(W),O,V);
}
EFI_STATUS MdpNativeResourcesWrite(MDP_NATIVE_RESOURCES *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 V){
 assert(R==&N.Resources);return Write(&H,NativeWindow(W),O,V);
}
EFI_STATUS MdpNativeResourcesFence(MDP_NATIVE_RESOURCES *R){assert(R==&N.Resources);return Fence(&H);}
EFI_STATUS MdpNativeResourcesDelay(MDP_NATIVE_RESOURCES *R,UINT32 Us){
 assert(R==&N.Resources);
 /* The real native RawDelay callback bounds each wait to 20 ms. */
 if(!Us||Us>20000)return EFI_INVALID_PARAMETER;
 return Delay(&H,Us);
}
EFI_STATUS MdpNativeResourcesSetDsiPlan(MDP_NATIVE_RESOURCES *R,UINT32 I,CONST CR_CLOCK_DSI_PLAN *P,BOOLEAN On,BOOLEAN *Owned){
 assert(R==&N.Resources&&!I&&(!On||P));EFI_STATUS S=Step(&H);if(S==EFI_SUCCESS){H.Clocks=On;*Owned=On;}return S;
}
EFI_STATUS MdpNativeResourcesPublishFrame(MDP_NATIVE_RESOURCES *R,VOID *Cpu,UINTN Bytes){
 assert(R==&N.Resources);++Publishes;return Publish(&H,Cpu,Bytes);
}
EFI_STATUS MdpNativeResourcesPublishCommand(MDP_NATIVE_RESOURCES *R,VOID *Cpu,UINTN Bytes){
 assert(R==&N.Resources);return Publish(&H,Cpu,Bytes);
}
EFI_STATUS MdpNativeResourcesClose(MDP_NATIVE_RESOURCES *R){assert(R==&N.Resources);ResourceClosed=TRUE;return EFI_UNSUPPORTED;}
static EFI_STATUS EFIAPI Retain(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *Name){
 assert(P==&NativeRpmh&&(!strcmp(Name,"bobc1")||!strcmp(Name,"ldoa14")||!strcmp(Name,"ldoc11")));
 H.Power=TRUE;return Step(&H);
}
static EFI_STATUS EFIAPI RetainFixed(EFI_RPMH_CR_PROTOCOL *P,CONST CHAR8 *Name,UINT32 Mv,UINT8 Mode){
 assert(!strcmp(Name,"bobc1")&&Mv==3296&&Mode==6);return Retain(P,Name);
}
static EFI_STATUS EFIAPI PinAcquire(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,CONST GpioConfigParams *C,BOOLEAN *Owned){
 assert(P==&Gpio&&Owner==&N.Panel&&(C->PinNumber==75||C->PinNumber==66));*Owned=TRUE;return Step(&H);
}
static EFI_STATUS EFIAPI PinSet(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,UINT16 Pin,UINT8 Value){
 assert(P==&Gpio&&Owner==&N.Panel&&Pin==75&&Value<=1);EFI_STATUS S=Step(&H);
 if(S==EFI_SUCCESS){ResetLevel=Value;H.Reset=Value;}return S;
}
static EFI_STATUS EFIAPI PinRead(EFI_GPIO_CR_PROTOCOL *P,VOID *Owner,UINT16 Pin,UINT8 *Value){
 assert(P==&Gpio&&Owner==&N.Panel&&Pin==75);*Value=ResetLevel;return Step(&H);
}
static EFI_STATUS EFIAPI PrepareObserver(EFI_SMMU_CR_PROTOCOL *P,VOID *Owner,EFI_SMMU_CR_EXIT_PREPARE F,VOID *C){
 assert(P==&NativeSmmu&&Owner==&N&&C==&N&&!NativePrepare&&F);NativePrepare=F;return Step(&H);
}
static VOID Complete(VOID *C,EFI_STATUS S){assert(C==&N&&!N.Busy&&EventTpl<=TPL_CALLBACK);++Notifications;NotificationStatus=S;}
static void NativeSetup(void){
 Init();EventSetup(115);memset(&N,0,sizeof(N));H.D=&N.Core;
 Notifications=Checks=Publishes=0;MappingLost=ResourceClosed=FALSE;ResetLevel=1;NativePrepare=NULL;
 Target=(CR_MDSS_TARGET){.Revision=CR_MDSS_TARGET_REVISION,.Size=sizeof(Target),.Soc=CR_MDSS_CMD_SM8250,
  .RequiresStoppedDisplay=TRUE,.VbifBase=0xaeb0000,.VbifBytes=0x3000,.Interrupt=115};
 NativeRpmh=(EFI_RPMH_CR_PROTOCOL){.Revision=EFI_RPMH_CR_PROTOCOL_REVISION,.EnsureRetainedVregEnabled=Retain,.EnsureRetainedVreg=RetainFixed};
 Gpio=(EFI_GPIO_CR_PROTOCOL){.Revision=EFI_GPIO_CR_PROTOCOL_REVISION,.AcquirePin=PinAcquire,.SetPinValue=PinSet,.ReadPinValue=PinRead};
 NativeSmmu=(EFI_SMMU_CR_PROTOCOL){.Revision=EFI_SMMU_CR_PROTOCOL_REVISION,.SetExitPrepareObserver=PrepareObserver};
 MDP_NATIVE_PROVIDERS P={.Rpmh=&NativeRpmh,.Smmu=&NativeSmmu};
 MDP_COMMAND_NATIVE_CONFIG C={.Resources={.Target=&Target},.Gpio=&Gpio,.NotifyContext=&N,.StartupComplete=Complete};
 assert(MdpPanelPowerR66451Config(&C.Panel)==EFI_SUCCESS);
 CR_MDSS_CMD_PROFILE F;CrMdssCmdR66451Profile(&F);
 EFI_STATUS OpenStatus=MdpCommandNativeOpen(&N,&P,&C,&F,0x82000000,sizeof(Frame));
 if(OpenStatus!=EFI_SUCCESS)fprintf(stderr,"open=%lx stage=%u panel=%u core=%u command=%p native=%p\n",(unsigned long)OpenStatus,N.Stage,N.Panel.Initialized,N.Core.Initialized,Commands,&N);
 assert(OpenStatus==EFI_SUCCESS&&N.ObserverBound&&N.Stage==3);
 assert(N.Profile.Config.Width==1080&&N.Profile.Config.Height==2340&&!N.Events.Initialized&&!Notifications);
}
static void NativeFirstFrame(void){
 H.Dpu[0x14/4]|=1U<<8;EventFire();assert(!Notifications&&!N.Core.Active);
 EventPump();assert(N.Core.Active&&Notifications==1&&NotificationStatus==EFI_SUCCESS&&EventEnabled);
 assert(MdpCommandNativeReady(&N)==EFI_SUCCESS&&Checks>=2);
}
int main(void){
 NativeSetup();assert(MdpCommandNativeStart(&N)==EFI_NOT_READY&&N.Core.Starting&&N.Events.Registered);
 assert(MdpCommandNativeStart(&N)==EFI_NOT_READY&&!Notifications&&!N.Core.Active);NativeFirstFrame();
 assert(MdpCommandNativePublish(&N,100,64)==EFI_SUCCESS&&Publishes>=2);
 assert(MdpCommandNativeBrightness(&N,256)==EFI_SUCCESS);
 MDP_COMMAND_NATIVE Copy=N;assert(MdpCommandNativeStart(&Copy)==EFI_INVALID_PARAMETER);
 MappingLost=TRUE;UINT32 Before=Publishes;
 assert(MdpCommandNativePublish(&N,0,4)==EFI_DEVICE_ERROR&&Publishes==Before);MappingLost=FALSE;
 /* A queued real notification prevents an EBS DMA-stop success. Drain its
    sealed callback, then the same retained owner completes physical freeze. */
 EventFire();EventTpl=TPL_NOTIFY;assert(NativePrepare(&N)==EFI_NOT_READY&&N.Frozen&&!N.Core.FreezeComplete);
 EventTpl=TPL_APPLICATION;EventPump();assert(!EventEnabled&&Notifications==1);
 EventTpl=TPL_NOTIFY;assert(NativePrepare(&N)==EFI_SUCCESS&&N.Core.FreezeComplete&&N.Events.Registered);
 assert(!EventCloses&&!EventUnregisters&&H.Power&&H.Clocks&&N.Panel.PermanentHold);
 assert(NativePrepare(&N)==EFI_SUCCESS);EventTpl=TPL_APPLICATION;
 assert(MdpCommandNativeStart(&N)==EFI_NOT_READY&&MdpCommandNativeClose(&N)==EFI_ACCESS_DENIED);
 NativeSetup();assert(MdpCommandNativeStart(&N)==EFI_NOT_READY);MappingLost=TRUE;
 H.Dpu[0x14/4]|=1U<<8;EventFire();EventPump();
 assert(!N.Core.Active&&Notifications==1&&NotificationStatus==EFI_DEVICE_ERROR&&N.Events.Faulted);
 NativeSetup();assert(MdpCommandNativeStart(&N)==EFI_NOT_READY);NativeFirstFrame();
 assert(MdpCommandNativeClose(&N)==EFI_UNSUPPORTED&&N.Initialized&&N.ObserverBound&&NativePrepare);
 puts("PASS: actual command native/core/panel/events/OSKAL first-frame IRQ, mapping loss, partial publication and retained EBS composition");return 0;
}
