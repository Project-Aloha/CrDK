/* Permanent enable floor composed with the actual supply-graph engine.
 * Provider writes are modeled, never called measurements. MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/RpmhRailLib.h>
#ifdef _KERNEL_MODE
static KIRQL CurrentIrql;
KIRQL KeGetCurrentIrql(VOID){return CurrentIrql;}
#endif
static UINT32 Writes,Fail;
static CR_STATUS Failure;
static UINT32 Values[3];
static CR_STATUS Address(VOID *Context,CONST CHAR8 *Name,UINT32 *Value){
  (void)Context;(void)Name;*Value=0x100;return CR_SUCCESS;
}
static CR_STATUS Write(VOID *Context,CONST CR_RPMH_RAIL *Rail,CR_RPMH_RAIL_VOTE_KIND Kind,UINT32 Value){
  (void)Context;assert(Rail->Address==0x100);Values[Kind]=Value;
  return ++Writes==Fail?Failure:CR_SUCCESS;
}
static CONST CR_RPMH_RAIL_IO Io={NULL,Address,Write};
static CR_RPMH_RAIL Rail;
static CR_RPMH_RAIL_RETAINED Owner;
static void Init(CONST CHAR8 *Name){
  Rail=(CR_RPMH_RAIL){.Name=Name};Owner=(CR_RPMH_RAIL_RETAINED){0};
  Writes=Fail=0;Failure=CR_DEVICE_ERROR;memset(Values,0,sizeof(Values));
}
static void ExistingBob(void){
  Init("bobc1");BOOLEAN Owned=FALSE;
  assert(CrRpmhRailAcquire(&Io,&Rail,3296,6,&Owned)==CR_SUCCESS&&Owned&&Rail.Ready);
  assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_SUCCESS);
  assert(Owner.Owned&&Owner.Rail==&Rail&&Owner.VoltageMv==3296&&Owner.Mode==6);
  assert(Rail.ReferenceCount==2&&Writes==3);
  assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_SUCCESS&&Writes==3&&Rail.ReferenceCount==2);
  assert(CrRpmhRailRelease(&Io,&Rail)==CR_SUCCESS&&Rail.ReferenceCount==1&&Writes==3);
  assert(CrRpmhRailAcquire(&Io,&Rail,3296,6,&Owned)==CR_SUCCESS&&Owned&&Rail.ReferenceCount==2);
  assert(CrRpmhRailAcquire(&Io,&Rail,3300,6,&Owned)==CR_BUSY&&!Owned);
  assert(CrRpmhRailRelease(&Io,&Rail)==CR_SUCCESS&&Rail.ReferenceCount==1&&Writes==3);
}
int main(void){
  ExistingBob();
  for(UINT32 Warning=0;Warning<2;++Warning)for(UINT32 At=1;At<=3;++At){
    Init("bobc1");Fail=At;Failure=Warning?1:CR_DEVICE_ERROR;BOOLEAN Owned=FALSE;
    CR_STATUS S=CrRpmhRailAcquire(&Io,&Rail,3296,6,&Owned);assert(Owned);
    /* Legacy Acquire may finish after an early warning; that is still no
     * exact applied grant for a new permanent retained reference. */
    assert(S==((Warning&&At<3)?CR_SUCCESS:Failure));
    UINT32 Before=Writes;Fail=0;
    assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_BUSY&&!Owner.Owned&&Writes==Before);
  }
  /* A successful fixed lease followed by a low-level drift, timeout,
   * warning or withdrawal must not become a permanent inherited floor. */
  for(UINT32 Kind=0;Kind<3;++Kind)for(UINT32 Return=0;Return<3;++Return){
    Init("bobc1");BOOLEAN Owned;
    assert(CrRpmhRailAcquire(&Io,&Rail,3296,6,&Owned)==CR_SUCCESS);
    Fail=Return?Writes+1:0;Failure=Return==2?1:CR_DEVICE_ERROR;
    assert(CrRpmhRailVote(&Io,&Rail,(CR_RPMH_RAIL_VOTE_KIND)Kind,Kind==0?3304:Kind==1?7:0)==(Return?Failure:CR_SUCCESS));
    UINT32 Before=Writes;
    assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_BUSY&&!Owner.Owned&&Writes==Before);
  }
  Init("smpb12");BOOLEAN Owned;
  assert(CrRpmhRailAcquire(&Io,&Rail,1352,6,&Owned)==CR_SUCCESS);
  assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_BUSY&&!Owner.Owned&&Writes==3);
  Init("bobc1");CR_RPMH_RAIL Parent={.Name="input"};Rail.Parent=&Parent;Rail.ParentMv=1352;Rail.ParentMode=6;
  assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_UNSUPPORTED&&!Owner.Owned&&!Writes);
  Init("bobc1");Rail.ReferenceCount=1;Rail.Ready=TRUE;Rail.VoltageRequested=Rail.ModeRequested=Rail.EnableRequested=TRUE;
  Rail.VoltageMv=3296;Rail.Mode=6;
  assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_BUSY&&!Owner.Owned&&!Writes);
  Init("ldoa14");Fail=1;assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_DEVICE_ERROR&&Owner.Owned);
  Fail=0;assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_SUCCESS&&Writes==2);
  assert(CrRpmhRailRetainEnable(&Io,&Rail,&Owner)==CR_SUCCESS&&Writes==2&&Rail.ReferenceCount==1);
  assert(!Values[CrRpmhRailVoltage]&&!Values[CrRpmhRailMode]&&Values[CrRpmhRailEnable]==1);
  puts("PASS: applied BOB reference pin, exact contract, stale/raw/partial rejection and enable-only positive retry");
  return 0;
}
