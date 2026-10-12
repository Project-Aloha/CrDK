/** SM8350 GCC resource adapter. SPDX-License-Identifier: MIT
 * Reference: pinned Linux gcc-sm8350 / clk-alpha-pll and Android gcc-lahaina.
 * Initialization does not ungate, retune, or touch hardware. Storage-only scope.
 */
#include <Library/CrTargetClockLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>

STATIC ClockController mGcc;
/* SM8350 GPLL0 uses Lucid5LPE: USER bit21 enables vote operation,
 * MODE bit31 acknowledges lock. This differs from SM8250 MODE FSM/ACTIVE.
 * We only vote an already configured 600MHz GPLL0 and its /2 output.
 * See LahainaCrPkg/Tests/sm8350-storage-facts.json; no analogue writes. */
STATIC ClockNode gpll0 = {
  .Name="gcc_gpll0", .Type=CLOCK_NODE_TYPE_FIXED_PLL, .ParentController=&mGcc,
  .FixedPll.Description={
    .VoteRegister=0x52018, .VoteMask=BIT(0),
    .ModeRegister=0, .UserRegister=0x0c, .LRegister=4, .AlphaRegister=0x40,
    .FsmMask=BIT(21), .LockMask=BIT(31),
    .PostDividerMask=15U<<8, .PostDividerValue=1U<<8,
    .ExpectedL=31, .ExpectedAlpha=0x4000,
    .ParentHz=19200000, .FrequencyHz=600000000, .FsmInModeRegister=FALSE,
  },
};
#include "CrClockGenerated.h"

STATIC ClockController mGcc = {
  .Address=0x00100000, .Size=0x1f0000,
  .Clks=mClocks, .ClkCount=ARRAY_SIZE(mClocks),
  .Gdscs=NULL, .GdscCount=0,
};
STATIC ClockDriverContext mContext={.ClockControllers=&mGcc,.ClockControllerCount=1};
STATIC BOOLEAN mInitialized;

STATIC CR_STATUS PrepareSource(ClockDriverContext *Context, ClockNode *Node,
                               CONST ClockRcgFreqTable *Freq) {
  if (Context!=&mContext || Node==NULL || Freq==NULL || Node->ParentController!=&mGcc)
    return CR_INVALID_PARAMETER;
  if (Node!=&gcc_sdcc2_apps_clk_src) return CR_UNSUPPORTED;
  if (Freq->Source.Config==0) return CR_SUCCESS;
  if (Freq->Source.Config!=1 && Freq->Source.Config!=6) return CR_UNSUPPORTED;
  /* A leaf acquires this PLL before its RCG. Direct RCG callers must hold
   * the same fixed-source lease; observing a locked foreign PLL is not ownership. */
  if (!gpll0.ReferenceCount || gpll0.ReleasePending) return CR_NOT_READY;
  return CrFixedPllCheck(&gpll0.FixedPll);
}

ClockDriverContext *CrTargetGetClockContext(VOID) { return &mContext; }
DebugccDriverContext *CrTargetGetDebugccContext(VOID) { return NULL; }
CR_STATUS CrTargetClockInit(ClockDriverContext *Context) {
  if (Context!=&mContext || !Context->LockInitialized ||
      Context->ClockControllers!=&mGcc || Context->ClockControllerCount!=1 ||
      mGcc.Address!=0x00100000 || mGcc.Size!=0x1f0000) return CR_INVALID_PARAMETER;
  if (mInitialized) return CR_SUCCESS;
  Context->PrepareSource=PrepareSource;
  mInitialized=TRUE;
  return CR_SUCCESS;
}

typedef struct { CONST CHAR8 *Name; UINT32 Offset; CONST CHAR8 *Clock; } RESET;
/* Source gcc_sm8350_resets. Only the qualified SDCC2 reset is exposed. */
STATIC CONST RESET mResets[]={
  {"gcc_sdcc2_bcr",0x14000,"gcc_sdcc2_ahb_clk"},
};
CR_STATUS CrTargetClockReset(CONST CHAR8 *Controller, CONST CHAR8 *Id, BOOLEAN Assert) {
  if (Controller==NULL || Id==NULL || Assert>1) return CR_INVALID_PARAMETER;
  if (cr_strcmp(Controller,"gcc")!=0) return CR_UNSUPPORTED;
  if (!mInitialized || !mContext.LockInitialized) return CR_NOT_READY;
  for (UINTN I=0; I<ARRAY_SIZE(mResets); ++I) {
    if (cr_strcmp(Id,mResets[I].Name)!=0) continue;
    UINTN Address=mGcc.Address+mResets[I].Offset;
    CrLockAcquire(&mContext.Lock);
    CrMmioUpdateBits32(Address,BIT(0),Assert?BIT(0):0);
    CrMemoryFence();
    CR_STATUS Status=(CrMmioRead32(Address)&BIT(0))==(Assert?BIT(0):0)?CR_SUCCESS:CR_DEVICE_ERROR;
    CrLockRelease(&mContext.Lock);
    return Status;
  }
  return CR_NOT_FOUND;
}
CR_STATUS CrTargetClockResolveResource(CONST CHAR8 *Controller, CONST CHAR8 *Id,
    CR_DAL_CLOCK_RESOURCE_KIND Kind, CONST CHAR8 **NativeId, UINT32 *Flags) {
  if (Controller==NULL || Id==NULL || NativeId==NULL || Flags==NULL || Kind>=CrDalClockResourceMax)
    return CR_INVALID_PARAMETER;
  *NativeId=NULL; *Flags=0;
  if (cr_strcmp(Controller,"gcc")!=0) return CR_UNSUPPORTED;
  if (Kind==CrDalClockResourceReset) {
    for (UINTN I=0; I<ARRAY_SIZE(mResets); ++I) if (cr_strcmp(Id,mResets[I].Name)==0) {
      *NativeId=mResets[I].Clock; return CR_SUCCESS;
    }
    return CR_NOT_FOUND;
  }
  ClockNode **Nodes=mClocks;
  UINTN Count=Kind==CrDalClockResourcePowerDomain?0:ARRAY_SIZE(mClocks);
  for (UINTN I=0; I<Count; ++I) if (Nodes[I]!=NULL && cr_strcmp(Id,Nodes[I]->Name)==0) {
    *NativeId=Nodes[I]->Name; return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
