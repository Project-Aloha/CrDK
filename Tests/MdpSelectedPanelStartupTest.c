/* Selected frontend completion contract; native backend and publication are
 * modeled. Executes the complete production translation unit. GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../Driver/MDSSCrDxe/MdpSelectedPanel.c"

static UINT32 BrightnessCalls, PublishCalls;
static EFI_STATUS BrightnessStatus, PublishStatus;
static BOOLEAN ExitDuringBrightness;
EFI_STATUS MdpCommandNativeBrightness(MDP_COMMAND_NATIVE *N,UINT32 Level) {
  assert(N==&mCommand && Level==1024 && Level<=4095);
  ++BrightnessCalls;
  if(ExitDuringBrightness)mOutput.Exiting=TRUE;
  return N->Core.Active?BrightnessStatus:EFI_NOT_READY;
}
EFI_STATUS MdpExtendedOutputPublishPipeline(MDP_EXTENDED_OUTPUT *O) {
  assert(O==&mOutput && BrightnessCalls==1 && BrightnessStatus==EFI_SUCCESS && !O->Exiting);
  ++PublishCalls;
  if(PublishStatus==EFI_SUCCESS)O->Published=TRUE;
  return PublishStatus;
}
BOOLEAN MdpHdkExtendedProfileRetained(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintEnabled(VOID){return FALSE;}
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L){(VOID)L;return FALSE;}
VOID EFIAPI DebugPrint(UINTN L,CONST CHAR8 *F,...){(VOID)L;(VOID)F;}

int main(int argc,char **argv) {
  assert(argc==2);int Case=atoi(argv[1]);
  mCommand.Core.Active=TRUE; mOutput.Initialized=TRUE;
  if(Case==0) {
    StartupComplete(&mOutput,EFI_SUCCESS);
    assert(BrightnessCalls==1 && PublishCalls==1 && mOutput.Published);
  } else if(Case==1) {
    StartupComplete(&mOutput,EFI_DEVICE_ERROR);
    assert(!BrightnessCalls && !PublishCalls);
  } else if(Case==2) {
    StartupComplete(NULL,EFI_SUCCESS);
    assert(!BrightnessCalls && !PublishCalls);
  } else if(Case==3) {
    mOutput.Exiting=TRUE;StartupComplete(&mOutput,EFI_SUCCESS);
    assert(!BrightnessCalls && !PublishCalls);
  } else if(Case==4 || Case==5) {
    BrightnessStatus=Case==4?EFI_DEVICE_ERROR:EFI_WARN_UNKNOWN_GLYPH;
    StartupComplete(&mOutput,EFI_SUCCESS);
    assert(BrightnessCalls==1 && !PublishCalls && !mOutput.Published);
  } else if(Case==6) {
    ExitDuringBrightness=TRUE;StartupComplete(&mOutput,EFI_SUCCESS);
    assert(BrightnessCalls==1 && !PublishCalls && !mOutput.Published);
  } else if(Case==7) {
    PublishStatus=EFI_DEVICE_ERROR;StartupComplete(&mOutput,EFI_SUCCESS);
    assert(BrightnessCalls==1 && PublishCalls==1 && !mOutput.Published);
  } else if(Case==8) {
    StartupComplete(&mOutput,EFI_SUCCESS);StartupComplete(&mOutput,EFI_SUCCESS);
    assert(BrightnessCalls==1 && PublishCalls==1);
  } else if(Case==9) {
    mCommand.Core.Active=FALSE;StartupComplete(&mOutput,EFI_SUCCESS);
    assert(BrightnessCalls==1 && !PublishCalls);
  } else if(Case==10) {
    StartupComplete(&mOutput,EFI_WARN_UNKNOWN_GLYPH);
    assert(!BrightnessCalls && !PublishCalls);
  } else { assert(0); }
  printf("selected-panel startup case %d PASS\n",Case);return 0;
}
