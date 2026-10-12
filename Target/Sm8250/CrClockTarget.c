/** SM8250 GCC resource adapter. SPDX-License-Identifier: MIT
 * Reference: pinned Linux gcc-sm8250 / clk-alpha-pll and Android gcc-kona.
 * Initialization reads DFS ownership and does not ungate or retune hardware.
 */
#include <Library/CrTargetClockLib.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>

STATIC ClockController mControllers[CC_TYPE_DISP_CC+1];
#define mGcc mControllers[CC_TYPE_GCC]
#define mDisp mControllers[CC_TYPE_DISP_CC]
STATIC CR_STATUS CheckDisplayPll(ClockNode *Node,UINT64 RateHz);
/* LUCID/TRION fixed PLL: MODE FSM bit20, ACTIVE acknowledgement bit30.
 * Firmware supplies the analogue configuration. Only APPS' vote is owned.
 * GPLL0 main 600 MHz and its observed /2 output feed the generated tables. */
STATIC ClockNode gpll0 = {
  .Name="gcc_gpll0", .Type=CLOCK_NODE_TYPE_FIXED_PLL, .ParentController=&mGcc,
  .FixedPll.Description={
    .VoteRegister=0x52018, .VoteMask=BIT(0),
    .ModeRegister=0, .UserRegister=0x0c, .LRegister=4, .AlphaRegister=0x40,
    .FsmMask=BIT(20), .LockMask=BIT(30),
    .PostDividerMask=15U<<8, .PostDividerValue=1U<<8,
    .ExpectedL=31, .ExpectedAlpha=0x4000,
    .ParentHz=19200000, .FrequencyHz=600000000, .FsmInModeRegister=TRUE,
  },
};
#include "CrClockGenerated.h"

STATIC ClockController mControllers[CC_TYPE_DISP_CC+1] = {
 [CC_TYPE_GCC] = {
  .Address=0x00100000, .Size=0x1f0000,
  .Clks=mClocks, .ClkCount=ARRAY_SIZE(mClocks),
  .Gdscs=mGdscs, .GdscCount=ARRAY_SIZE(mGdscs),
 },
 [CC_TYPE_DISP_CC] = {
  .Address=0xaf00000,.Size=0x20000,
  .Clks=mDisplayClocks,.ClkCount=ARRAY_SIZE(mDisplayClocks),
  .Gdscs=mDisplayGdscs,.GdscCount=ARRAY_SIZE(mDisplayGdscs),
 },
};
STATIC ClockDriverContext mContext={.ClockControllers=mControllers,.ClockControllerCount=ARRAY_SIZE(mControllers)};
STATIC BOOLEAN mInitialized;
#define CR_DISPLAY_PLL_FSM_IN_MODE TRUE
#include "../Common/CrDisplayClockShared.h"
#undef CR_DISPLAY_PLL_FSM_IN_MODE

/* QUP uses hardware DFS when firmware sets CMD+0x14 bit0. Linux uses
 * read-only DFS ops in that case. Omit S1/S4; S5 instead borrows the exact
 * inherited table0 XO contract below without changing the DFS table. */
STATIC VOID OmitDfsClock(UINT32 SourceId, UINT32 BranchId) {
  ClockNode *Source=mClocks[SourceId];
  if (Source!=NULL && (CrMmioRead32(mGcc.Address+Source->CmdRegister+0x14)&BIT(0))) {
    mClocks[SourceId]=NULL;
    mClocks[BranchId]=NULL;
  }
}
/* Android Kona FIFO I2C selects SE_GENI_CLK_SEL0. Public clk-rcg2 DFS ops
 * observe the firmware's table rather than writing CMD/CFG or clearing DFS.
 * Qualify only table0's exact XO19.2MHz contract: source0, bypass fractional
 * mode and HID0/1 (both decode /1). Other parents/rates stay unsupported. */
STATIC CR_STATUS CheckTouchDfsSource(ClockNode *Node,UINT64 RateHz) {
  if(Node!=&gcc_qupv3_wrap1_s5_clk_src || Node->ParentController!=&mGcc ||
     Node->CmdRegister!=0x18600 || !mInitialized || !mContext.LockInitialized)
    return CR_INVALID_PARAMETER;
  if(RateHz!=19200000)return CR_UNSUPPORTED;
  UINTN Base=mGcc.Address+Node->CmdRegister;
  UINT32 Control=CrMmioRead32(Base+0x14),Config=CrMmioRead32(Base+0x1c);
  if(!(Control&BIT(0)) || (Config&(CLOCK_NODE_RCG_CMD_REGISTER_CFG_SRC_SEL_MSK |
       CLOCK_NODE_RCG_CMD_REGISTER_CFG_MODE_MSK)) || (Config&31U)>1U)
    return CR_UNSUPPORTED;
  /* A concurrent firmware change must not certify an unstable table. */
  if(Control!=CrMmioRead32(Base+0x14) || Config!=CrMmioRead32(Base+0x1c))
    return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS PrepareSource(ClockDriverContext *Context, ClockNode *Node,
                               CONST ClockRcgFreqTable *Freq) {
  if (Context!=&mContext || Node==NULL || Freq==NULL) return CR_INVALID_PARAMETER;
  if(Node->ParentController==&mDisp)return PrepareDisplaySource(Node,Freq);
  if(Node->ParentController!=&mGcc)return CR_INVALID_PARAMETER;
  if ((Node==&gcc_qupv3_wrap0_s1_clk_src || Node==&gcc_qupv3_wrap1_s4_clk_src || Node==&gcc_qupv3_wrap1_s5_clk_src) &&
      (CrMmioRead32(mGcc.Address+Node->CmdRegister+0x14)&BIT(0)))
    return CR_UNSUPPORTED;
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
      Context->ClockControllers!=mControllers || Context->ClockControllerCount!=ARRAY_SIZE(mControllers) ||
      mGcc.Address!=0x00100000 || mGcc.Size!=0x1f0000 ||
      mDisp.Address!=0xaf00000 || mDisp.Size!=0x20000) return CR_INVALID_PARAMETER;
  if (mInitialized) return CR_SUCCESS;
  OmitDfsClock(SM8250_GCC_QUPV3_WRAP0_S1_CLK_SRC,SM8250_GCC_QUPV3_WRAP0_S1_CLK);
  OmitDfsClock(SM8250_GCC_QUPV3_WRAP1_S4_CLK_SRC,SM8250_GCC_QUPV3_WRAP1_S4_CLK);
  if(CrMmioRead32(mGcc.Address+gcc_qupv3_wrap1_s5_clk_src.CmdRegister+0x14)&BIT(0)) {
    gcc_qupv3_wrap1_s5_clk_src.Type=CLOCK_NODE_TYPE_READ_ONLY_SOURCE;
    gcc_qupv3_wrap1_s5_clk_src.ReadOnlySourceCheck=CheckTouchDfsSource;
    gcc_qupv3_wrap1_s5_clk_src.FixedRateHz=19200000;
  }
  Context->PrepareSource=PrepareSource;
  mInitialized=TRUE;
  return CR_SUCCESS;
}

typedef struct { CONST CHAR8 *Name; UINT32 Offset; CONST CHAR8 *Clock; } RESET;
/* Source gcc_sm8250_resets. No wrapper reset is exposed: UART12 shares QUP1. */
STATIC CONST RESET mResets[]={
  {"gcc_pcie_0_bcr",0x6b000,"gcc_pcie_0_cfg_ahb_clk"},
  {"gcc_pcie_0_phy_bcr",0x6c01c,"gcc_pcie_0_cfg_ahb_clk"},
  {"gcc_pcie_1_bcr",0x8d000,"gcc_pcie_1_cfg_ahb_clk"},
  {"gcc_pcie_1_phy_bcr",0x8e01c,"gcc_pcie_1_cfg_ahb_clk"},
  {"gcc_pcie_2_bcr",0x6000,"gcc_pcie_2_cfg_ahb_clk"},
  {"gcc_pcie_2_phy_bcr",0x1f01c,"gcc_pcie_2_cfg_ahb_clk"},
  {"gcc_sdcc2_bcr",0x14000,"gcc_sdcc2_ahb_clk"},
  {"gcc_ufs_phy_bcr",0x77000,"gcc_ufs_phy_ahb_clk"},
  {"gcc_usb30_prim_bcr",0xf000,"gcc_usb30_prim_master_clk"},
  {"gcc_usb30_sec_bcr",0x10000,"gcc_usb30_sec_master_clk"},
  {"gcc_qusb2phy_prim_bcr",0x12000,"gcc_usb30_prim_mock_utmi_clk"},
  {"gcc_qusb2phy_sec_bcr",0x12004,"gcc_usb30_sec_mock_utmi_clk"},
  {"gcc_usb3_dp_phy_prim_bcr",0x50008,"gcc_usb3_prim_phy_aux_clk"},
  {"gcc_usb3_dp_phy_sec_bcr",0x50014,"gcc_usb3_sec_phy_aux_clk"},
  {"gcc_usb3_phy_prim_bcr",0x50000,"gcc_usb3_prim_phy_aux_clk"},
  {"gcc_usb3_phy_sec_bcr",0x5000c,"gcc_usb3_sec_phy_aux_clk"},
  {"gcc_usb3phy_phy_prim_bcr",0x50004,"gcc_usb3_prim_phy_aux_clk"},
  {"gcc_usb3phy_phy_sec_bcr",0x50010,"gcc_usb3_sec_phy_aux_clk"},
};
CR_STATUS CrTargetClockReset(CONST CHAR8 *Controller, CONST CHAR8 *Id, BOOLEAN Assert) {
  if (Controller==NULL || Id==NULL || Assert>1) return CR_INVALID_PARAMETER;
  if(!cr_strcmp(Controller,"dispcc"))return ResetDisplayCore(Id,Assert);
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
  if(!cr_strcmp(Controller,"dispcc"))return ResolveDisplayResource(Id,Kind,NativeId,Flags);
  if (cr_strcmp(Controller,"gcc")!=0) return CR_UNSUPPORTED;
  if (Kind==CrDalClockResourceReset) {
    for (UINTN I=0; I<ARRAY_SIZE(mResets); ++I) if (cr_strcmp(Id,mResets[I].Name)==0) {
      *NativeId=mResets[I].Clock; return CR_SUCCESS;
    }
    return CR_NOT_FOUND;
  }
  ClockNode **Nodes=Kind==CrDalClockResourcePowerDomain?mGdscs:mClocks;
  UINTN Count=Kind==CrDalClockResourcePowerDomain?ARRAY_SIZE(mGdscs):ARRAY_SIZE(mClocks);
  for (UINTN I=0; I<Count; ++I) if (Nodes[I]!=NULL && cr_strcmp(Id,Nodes[I]->Name)==0) {
    *NativeId=Nodes[I]->Name;
    if (Nodes[I]==&gcc_pcie_0_pipe_clk || Nodes[I]==&gcc_pcie_1_pipe_clk || Nodes[I]==&gcc_pcie_2_pipe_clk)
      *Flags=CR_DAL_CLOCK_RESOURCE_NO_RATE;
    return CR_SUCCESS;
  }
  return CR_NOT_FOUND;
}
