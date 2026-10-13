/* One native frontend selects an explicit command or video panel profile.
 * Hardware owners and the physical GOP implementation are shared.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpSelectedPanel.h"
#include "MdpHdkExtendedProfile.h"
#include "MdpCommandNative.h"
#include "MdpExtendedOutput.h"
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>

/* Raw 12-bit panel level, not a measured luminance percentage. Only the
 * explicitly selected R66451 command route uses this boot policy. */
#define R66451_BOOT_BRIGHTNESS 1024U

STATIC UINT32 mSelection;
STATIC MDP_COMMAND_NATIVE mCommand;
STATIC MDP_EXTENDED_OUTPUT mOutput;
STATIC CR_MDSS_CMD_PROFILE mCommandProfile;
STATIC EFI_STATUS Exact(EFI_STATUS S){return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC EFI_STATUS Locate(CONST EFI_GUID *Guid,VOID **Out){
  VOID *P=NULL;EFI_STATUS S=Exact(gBS->LocateProtocol((EFI_GUID *)Guid,NULL,&P));
  if(S!=EFI_SUCCESS)return S;if(!P)return EFI_NOT_READY;*Out=P;return EFI_SUCCESS;
}
STATIC EFI_STATUS Ready(VOID *C){
  MDP_COMMAND_NATIVE *N=C;EFI_STATUS S=MdpCommandNativeReady(N);
  return S==EFI_SUCCESS?MdpNativeResourcesCheck(&N->Resources):S;
}
STATIC EFI_STATUS Publish(VOID *C,UINTN Offset,UINTN Bytes){return MdpCommandNativePublish(C,Offset,Bytes);}
STATIC EFI_STATUS Close(VOID *C){MDP_COMMAND_NATIVE *N=C;return N->Initialized?MdpCommandNativeClose(N):EFI_SUCCESS;}
STATIC EFI_STATUS SetPower(VOID *C,EFI_DISPLAY_POWER_STATE State){
  /* Selected SWIRE power has no qualified physical off contract. */
  return State==EfiDisplayPowerStateOn?MdpCommandNativeStart(C):EFI_UNSUPPORTED;
}
STATIC EFI_STATUS GetPower(VOID *C,EFI_DISPLAY_POWER_STATE *State){
  EFI_STATUS S=MdpCommandNativeReady(C);if(S==EFI_SUCCESS)*State=EfiDisplayPowerStateOn;return S;
}
STATIC VOID StartupComplete(VOID *C,EFI_STATUS Status){
  MDP_EXTENDED_OUTPUT *O=C;
  if(O!=&mOutput||O->Exiting||O->Published)return;
  if(Status!=EFI_SUCCESS){
    DEBUG((DEBUG_ERROR,"MDSSCr: command first-frame completion %r stage=%u retained=%u\n",
      Status,mCommand.Stage,MdpSelectedPanelRetained()));
    return;
  }
  /* The stock sleep-out/display-on sequence does not set DCS brightness.
   * This callback runs after the first real frame and outside the core gate;
   * use the existing serialized command owner, not a direct DSI write. */
  EFI_STATUS S=Exact(MdpCommandNativeBrightness(&mCommand,R66451_BOOT_BRIGHTNESS));
  DEBUG((S==EFI_SUCCESS?DEBUG_WARN:DEBUG_ERROR,
    "MDSSCr: R66451 boot brightness level=%u status=%r\n",R66451_BOOT_BRIGHTNESS,S));
  if(S!=EFI_SUCCESS||O->Exiting)return;
  S=MdpExtendedOutputPublishPipeline(O);
  DEBUG((S==EFI_SUCCESS?DEBUG_WARN:DEBUG_ERROR,
    "MDSSCr: command first-frame GOP publication %r %ux%u CPU=%lx IOVA=%lx\n",
    S,O->Info.HorizontalResolution,O->Info.VerticalResolution,O->Mode.FrameBufferBase,mCommand.Resources.FrameIova));
}
BOOLEAN MdpSelectedPanelRetained(VOID){
  return mOutput.Initialized||mCommand.Initialized||MdpHdkExtendedProfileRetained();
}
EFI_STATUS MdpSelectedPanelStart(UINT32 Selection){
  if(Selection!=MDP_SELECTED_R66451_COMMAND&&Selection!=MDP_SELECTED_LT9611_VIDEO)return EFI_UNSUPPORTED;
  if(mSelection)return EFI_ALREADY_STARTED;
  if(EfiGetCurrentTpl()>TPL_CALLBACK)return EFI_ACCESS_DENIED;
  if(Selection==MDP_SELECTED_LT9611_VIDEO){
    EFI_STATUS S=MdpHdkExtendedProfileStart();if(MdpHdkExtendedProfileRetained())mSelection=Selection;return S;
  }
  /* Selection is a board build contract. Soc identity alone never selects an
     arbitrary attached panel; the profile and power description are exact. */
  CONST CR_MDSS_TARGET *Target=CrDalGetMdssContext();
  if(!Target||Target->Soc!=CR_MDSS_CMD_SM8250||!Target->RequiresStoppedDisplay)return EFI_UNSUPPORTED;
  CrMdssCmdR66451Profile(&mCommandProfile);
  CR_MDSS_CMD_PLAN Plan;EFI_STATUS S=CrMdssCmdBuildPlanProfile(&mCommandProfile,&Plan);if(S!=EFI_SUCCESS)return S;
  ARM_MEMORY_REGION_DESCRIPTOR_EX Region;
  S=Exact(LocateMemoryMapAreaByName("Display Reserved",&Region));if(S!=EFI_SUCCESS)return S;
  UINT64 Bytes=(UINT64)mCommandProfile.Config.Stride*mCommandProfile.Config.Height;
  if(!Region.Address||Bytes>Region.Length||Bytes>MAX_UINTN)return EFI_BAD_BUFFER_SIZE;
  MDP_NATIVE_PROVIDERS P={.Dxe=gDS};EFI_GPIO_CR_PROTOCOL *Gpio=NULL;
#define GET(Member,Guid) do{S=Locate(&(Guid),(VOID **)&P.Member);if(S!=EFI_SUCCESS)return S;}while(0)
  GET(Clock,gEfiClockCrProtocolGuid);GET(External,gEfiClockExternalCrProtocolGuid);
  GET(Rpmh,gEfiRpmhCrProtocolGuid);GET(Icb,gEfiInterconnectCrProtocolGuid);
  GET(Smmu,gEfiSmmuCrProtocolGuid);GET(Iommu,gEdkiiIoMmuProtocolGuid);
#undef GET
  S=Locate(&gEfiGpioCrProtocolGuid,(VOID **)&Gpio);if(S!=EFI_SUCCESS)return S;
  UINT64 Average=(Bytes*mCommandProfile.Config.RefreshHz+999)/1000;
  MDP_COMMAND_NATIVE_CONFIG Config={.Resources={.Target=Target,.MdpHz=200000000,
    .AverageKBps={1000,Average},.PeakKBps={1000,Average*2}},.Gpio=Gpio,
    .NotifyContext=&mOutput,.StartupComplete=StartupComplete};
  S=MdpPanelPowerR66451Config(&Config.Panel);if(S!=EFI_SUCCESS)return S;
  CONST MDP_OUTPUT_PIPELINE Pipeline={Ready,Publish,MdpCommandNativeFreeze,Close,SetPower,GetPower};
  /* Deferred frame completion cannot publish while preparation owns the
     output. Real IRQs remain enabled; their callback runs after this gate. */
  EFI_TPL Old=gBS->RaiseTPL(TPL_CALLBACK);
  S=MdpExtendedOutputInitPipeline(&mOutput,P.Dxe,&Pipeline,&mCommand,Region.Address,
    mCommandProfile.Config.Width,mCommandProfile.Config.Height,mCommandProfile.Config.Stride);
  if(mOutput.Initialized)mSelection=Selection;
  if(S==EFI_SUCCESS)S=MdpCommandNativeOpen(&mCommand,&P,&Config,&mCommandProfile,Region.Address,(UINTN)Bytes);
  if(S==EFI_SUCCESS)S=MdpExtendedOutputBindPrepare(&mOutput,P.Smmu);
  if(S==EFI_SUCCESS)S=MdpExtendedOutputConfigureFramebuffer(&mOutput);
  if(S==EFI_SUCCESS)S=MdpCommandNativeStart(&mCommand);
  if(S!=EFI_SUCCESS&&!(S==EFI_NOT_READY&&mCommand.Core.Starting))
    DEBUG((DEBUG_ERROR,"MDSSCr: selected command startup %r stage=%u retained=%u\n",S,mCommand.Stage,MdpSelectedPanelRetained()));
  gBS->RestoreTPL(Old);
  if(mOutput.Published)return EFI_SUCCESS;
  return S;
}
EFI_STATUS MdpSelectedPanelUnload(VOID){
  if(mSelection==MDP_SELECTED_LT9611_VIDEO)return MdpHdkExtendedProfileUnload();
  if(!mSelection)return EFI_SUCCESS;
  EFI_STATUS S=MdpExtendedOutputClose(&mOutput);if(S==EFI_SUCCESS)mSelection=0;return S;
}
