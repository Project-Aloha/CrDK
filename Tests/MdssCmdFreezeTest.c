/* Actual portable command EBS freeze with physical MMIO and failure controls.
 * The real core is shared with the lifecycle oracle; suppliers stay live. */
#define main PreviousLifecycleMain
#include "MdssCmdLifecycleTest.c"
#undef main
static CR_STATUS FreezeEvents(VOID *C,BOOLEAN *Frozen){
  HW *M=C;CR_STATUS S=Step(M);*Frozen=FALSE;
  if(M->IrqBusy)return CR_NOT_READY;
  if(S==CR_SUCCESS||M->Posted){M->Irq=FALSE;*Frozen=TRUE;}return S;
}
static void BeginFreeze(void){
  Init();D.Io.FreezeEvents=FreezeEvents;
  assert(CrMdssCmdStart(&D)==CR_NOT_READY);FirstFrame();Queued();H.Calls=0;
}
static void RetainedFreeze(void){
  assert(D.Frozen&&D.FreezeComplete&&D.IrqOwned&&D.IrqFrozen&&D.ExternalOwned&&D.PanelOwned);
  assert(H.Power&&H.Clocks&&H.Reset&&!H.Irq&&!D.DpuTouched&&!D.HostTouched&&!D.FrameInFlight&&!D.CommandInFlight);
  assert((H.Vbif[0x200/4]&2)&&(H.Vbif[0x204/4]&2));
  assert(CrMdssCmdStart(&D)==CR_NOT_READY&&CrMdssCmdStop(&D)==CR_NOT_READY&&CrMdssCmdUninit(&D)==CR_NOT_READY);
  assert(CrMdssCmdPublish(&D,0,4)==CR_NOT_READY&&CrMdssCmdSetBrightness(&D,1)==CR_NOT_READY);
}
int main(void){
  BeginFreeze();assert(CrMdssCmdFreeze(&D)==CR_SUCCESS);UINT32 Count=H.Calls;RetainedFreeze();
  assert(CrMdssCmdFreeze(&D)==CR_SUCCESS);RetainedFreeze();
  H.Host[4/4]|=1;assert(CrMdssCmdFreeze(&D)==CR_NOT_READY&&!D.FreezeComplete);
  H.Host[4/4]&=~1U;assert(CrMdssCmdFreeze(&D)==CR_SUCCESS);RetainedFreeze();
  for(UINT32 Warning=0;Warning<2;++Warning)for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 At=1;At<=Count;++At){
    BeginFreeze();H.Fail=At;H.Failure=Warning?1:CR_DEVICE_ERROR;H.Posted=Posted;
    assert(CrMdssCmdFreeze(&D)!=CR_SUCCESS&&D.Frozen&&!D.FreezeComplete);
    assert(H.Power&&H.Clocks&&H.Reset&&D.PanelOwned&&D.ExternalOwned&&D.IrqOwned);
    H.Fail=0;assert(CrMdssCmdFreeze(&D)==CR_SUCCESS);RetainedFreeze();
  }
  BeginFreeze();H.IrqBusy=TRUE;
  assert(CrMdssCmdFreeze(&D)==CR_NOT_READY&&D.Frozen&&!D.IrqFrozen&&D.FrameInFlight);
  H.IrqBusy=FALSE;assert(CrMdssCmdFreeze(&D)==CR_SUCCESS);RetainedFreeze();
  Init();assert(CrMdssCmdFreeze(&D)==CR_UNSUPPORTED&&!D.Frozen);
  Init();D.Io.FreezeEvents=FreezeEvents;
  assert(CrMdssCmdFreeze(&D)==CR_SUCCESS&&D.FreezeComplete&&!H.Power&&!H.Clocks);
  printf("PASS: actual command EBS freeze %u positions x4, same-owner retries, retained suppliers/IRQ lifetime, repeated physical fence and pre-start stop\n",Count);
  return 0;
}
