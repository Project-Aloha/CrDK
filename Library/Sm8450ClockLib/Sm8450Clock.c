/**
 * SM8450 GCC clock and GDSC target data used by Crane resource owners.
 *
 * The Linux GCC provider exposes a sparse clock array with public IDs 1..184.
 * CrClockGenerated.h is generated from the Linux binding/provider and keeps
 * the complete ID/name inventory synchronized.  Only clocks with native
 * register ownership are instantiated below; all other inventory entries
 * remain NULL in the operational map until their owner is implemented.
 * Resource ownership remains in ClockLib; this file only describes registers.
 */

#include <Library/CrSm8450ClockLib.h>
#include <oskal/common.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>
#include <oskal/cr_time.h>


#include <Library/CrSm8450ClockIds.h>

#define SM8450_CLOCK_CONTROLLER_COUNT  CC_TYPE_MAX

enum Sm8450ClockRcg2Sources {
  SM8450_CLOCK_RCG2_SRC_BI_TCXO       = 0,
  /* These values are the GCC parent-map mux values, not DT indices. */
  SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN    = 1,
  /* DISP CC parent-map value 1 is DISP PLL0 (same encoding). */
  SM8450_CLOCK_RCG2_SRC_DISP_PLL0      = 1,
  SM8450_CLOCK_RCG2_SRC_SLEEP_CLK     = 5,
  SM8450_CLOCK_RCG2_SRC_GPLL0_EVEN    = 6,
};

STATIC ClockController
    Sm8450ClockControllers[SM8450_CLOCK_CONTROLLER_COUNT];

STATIC ClockRcgFreqTable mPcieAuxFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_BI_TCXO, 0, 19200000, 1, 0, 0),
};

STATIC ClockRcgFreqTable mPcieRchngFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_BI_TCXO, 0, 19200000, 1, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN, 1, 100000000, 6, 0, 0),
};

STATIC ClockRcgFreqTable mQupS1Freq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_BI_TCXO, 0, 19200000, 1, 0, 0),
};

STATIC ClockRcgFreqTable mUfsCoreFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_GPLL0_EVEN, 6, 75000000, 4, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN, 1, 150000000, 4, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN, 1, 300000000, 2, 0, 0),
};

STATIC ClockRcgFreqTable mUfsAuxFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_BI_TCXO, 0, 19200000, 1, 0, 0),
};

/* HDK8450 initial SDR SDCC2: Linux gcc-sm8450 fixed GPLL0 and table.
 * Retain the local PLL vote before programming its dependent RCG. The
 * fixed source refuses a different firmware rate/divider or direct mode;
 * it never retunes an analogue PLL shared with other firmware consumers. */
STATIC ClockNode GccGpll0Fixed = {
  .Name = "gcc_gpll0", .Type = CLOCK_NODE_TYPE_FIXED_PLL,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
  .FixedPll.Description = {
    .VoteRegister = 0x62018, .VoteMask = BIT(0),
    .ModeRegister = 0, .UserRegister = 0x18,
    .LRegister = 0x10, .AlphaRegister = 0x14,
    .FsmMask = BIT(25), .LockMask = BIT(31), .CalibrationPendingMask = BIT(8),
    .PostDividerMask = 15U << 10, .PostDividerValue = 1U << 10,
    .ExpectedL = 31, .ExpectedAlpha = 0x4000,
    .ParentHz = 19200000, .FrequencyHz = 600000000,
  },
};
STATIC ClockRcgFreqTable mSdcc2AppsFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(SM8450_CLOCK_RCG2_SRC_BI_TCXO,0,400000,12,1,4),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(SM8450_CLOCK_RCG2_SRC_GPLL0_EVEN,6,25000000,12,0,0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(SM8450_CLOCK_RCG2_SRC_GPLL0_EVEN,6,50000000,6,0,0),
};
STATIC ClockNode GccSdcc2AppsSrc = {
  .Name = "gcc_sdcc2_apps_clk_src", .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister = 0x24014, .MNDWidth = 8, .HIDWidth = 5, .HwClockCtrl = TRUE,
  .FreqCount = ARRAY_SIZE(mSdcc2AppsFreq), .FreqTable = mSdcc2AppsFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode *mSdcc2AppsParents[] = { &GccGpll0Fixed, &GccSdcc2AppsSrc };
STATIC ClockNode GccSdcc2Ahb = {
  .Name = "gcc_sdcc2_ahb_clk", .Type = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister = 0x2400c, .EnableRegister = 0x2400c, .EnableMsk = BIT(0),
  .HaltCheckFlag = CLOCK_NODE_BRANCH_HALT,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccSdcc2Apps = {
  .Name = "gcc_sdcc2_apps_clk", .Type = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister = 0x24004, .EnableRegister = 0x24004, .EnableMsk = BIT(0),
  .HaltCheckFlag = CLOCK_NODE_BRANCH_HALT,
  .ParentCount = ARRAY_SIZE(mSdcc2AppsParents), .Parents = mSdcc2AppsParents,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccQup1S1ClkSrc = {
  .Name             = "gcc_qupv3_wrap1_s1_clk_src",
  .Type             = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister      = 0x28148,
  .MNDWidth         = 16,
  .HIDWidth         = 5,
  .HwClockCtrl      = TRUE,
  .FreqCount        = ARRAY_SIZE(mQupS1Freq),
  .FreqTable        = mQupS1Freq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccQup0S5ClkSrc = {
  .Name             = "gcc_qupv3_wrap0_s5_clk_src",
  .Type             = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister      = 0x27618,
  .MNDWidth         = 16,
  .HIDWidth         = 5,
  .HwClockCtrl      = TRUE,
  .FreqCount        = ARRAY_SIZE(mQupS1Freq),
  .FreqTable        = mQupS1Freq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

/* Linux gcc-sm8450.c: FIFO serial engines at the fixed XO rate. These
 * operational nodes do not load GENI firmware or authorize a native writer. */
#define QUP_FIFO_RCG(Node, NodeName, Cmd) \
  STATIC ClockNode Node = { \
    .Name=NodeName, .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, \
    .CmdRegister=Cmd, .MNDWidth=16, .HIDWidth=5, .HwClockCtrl=TRUE, \
    .FreqCount=ARRAY_SIZE(mQupS1Freq), .FreqTable=mQupS1Freq, \
    .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC], \
  }
QUP_FIFO_RCG(GccQup0S1Src, "gcc_qupv3_wrap0_s1_clk_src", 0x27148);
QUP_FIFO_RCG(GccQup0S3Src, "gcc_qupv3_wrap0_s3_clk_src", 0x273b0);
/* Linux S7 table: 300 MHz GPLL0 even output * 384 / 15625 =
 * 7.3728 MHz, exactly 115200 baud at sampling16/divider4.  Keep XO
 * as an explicit alternative; no shared analogue PLL retuning. */
STATIC ClockRcgFreqTable mQupS7Freq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(SM8450_CLOCK_RCG2_SRC_GPLL0_EVEN,6,7372800,1,384,15625),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(SM8450_CLOCK_RCG2_SRC_BI_TCXO,0,19200000,1,0,0),
};
STATIC ClockNode GccQup0S7Src = {
  .Name="gcc_qupv3_wrap0_s7_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister=0x27880, .MNDWidth=16, .HIDWidth=5, .HwClockCtrl=TRUE,
  .FreqCount=ARRAY_SIZE(mQupS7Freq), .FreqTable=mQupS7Freq,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
};
QUP_FIFO_RCG(GccQup1S0Src, "gcc_qupv3_wrap1_s0_clk_src", 0x28014);
QUP_FIFO_RCG(GccQup1S2Src, "gcc_qupv3_wrap1_s2_clk_src", 0x2827c);
QUP_FIFO_RCG(GccQup1S5Src, "gcc_qupv3_wrap1_s5_clk_src", 0x28618);
#undef QUP_FIFO_RCG

#define UFS_BRANCH(Node, NodeName, Halt, Enable, Mask, Flags, Hwcg, ParentList, ParentsCount) \
  STATIC ClockNode Node = { \
    .Name = NodeName, .Type = CLOCK_NODE_TYPE_BRANCH_2, \
    .HaltRegister = Halt, .HaltCheckFlag = Flags, \
    .HwcgRegister = Hwcg, .HwcgMsk = (Hwcg) != 0 ? BIT(1) : 0, \
    .EnableRegister = Enable, .EnableMsk = Mask, \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC], \
    .ParentCount = ParentsCount, .Parents = ParentList, \
  }

STATIC ClockNode GccUfsPhyAxiClkSrc = {
  .Name = "gcc_ufs_phy_axi_clk_src", .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister = 0x8702c, .MNDWidth = 8, .HIDWidth = 5, .HwClockCtrl = TRUE,
  .FreqCount = ARRAY_SIZE(mUfsCoreFreq), .FreqTable = mUfsCoreFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccUfsPhyIceCoreClkSrc = {
  .Name = "gcc_ufs_phy_ice_core_clk_src", .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister = 0x87074, .HIDWidth = 5, .HwClockCtrl = TRUE,
  .FreqCount = ARRAY_SIZE(mUfsCoreFreq), .FreqTable = mUfsCoreFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccUfsPhyAuxClkSrc = {
  .Name = "gcc_ufs_phy_phy_aux_clk_src", .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister = 0x870a8, .HIDWidth = 5, .HwClockCtrl = TRUE,
  .FreqCount = ARRAY_SIZE(mUfsAuxFreq), .FreqTable = mUfsAuxFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccUfsPhyUniproClkSrc = {
  .Name = "gcc_ufs_phy_unipro_core_clk_src", .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister = 0x8708c, .HIDWidth = 5, .HwClockCtrl = TRUE,
  .FreqCount = ARRAY_SIZE(mUfsCoreFreq), .FreqTable = mUfsCoreFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode *mQupS1BranchParents[] = { &GccQup1S1ClkSrc };
STATIC ClockNode *mQupS5BranchParents[] = { &GccQup0S5ClkSrc };
STATIC ClockNode *mUfsAxiBranchParents[] = { &GccUfsPhyAxiClkSrc };
STATIC ClockNode *mUfsIceBranchParents[] = { &GccUfsPhyIceCoreClkSrc };
STATIC ClockNode *mUfsAuxBranchParents[] = { &GccUfsPhyAuxClkSrc };
STATIC ClockNode *mUfsUniproBranchParents[] = { &GccUfsPhyUniproClkSrc };

UFS_BRANCH(GccQup1MAhbClk, "gcc_qupv3_wrap_1_m_ahb_clk", 0x28004, 0x62008,
           BIT(20), CLOCK_NODE_BRANCH_VOTED, 0x28004, NULL, 0);
UFS_BRANCH(GccQup1SAhbClk, "gcc_qupv3_wrap_1_s_ahb_clk", 0x28008, 0x62008,
           BIT(21), CLOCK_NODE_BRANCH_VOTED, 0x28008, NULL, 0);
UFS_BRANCH(GccQup1S1Clk, "gcc_qupv3_wrap1_s1_clk", 0x28140, 0x62008,
           BIT(23), CLOCK_NODE_BRANCH_VOTED, 0x28140, mQupS1BranchParents, ARRAY_SIZE(mQupS1BranchParents));
UFS_BRANCH(GccQup0MAhbClk, "gcc_qupv3_wrap_0_m_ahb_clk", 0x27004, 0x62008,
           BIT(6), CLOCK_NODE_BRANCH_VOTED, 0x27004, NULL, 0);
UFS_BRANCH(GccQup0SAhbClk, "gcc_qupv3_wrap_0_s_ahb_clk", 0x27008, 0x62008,
           BIT(7), CLOCK_NODE_BRANCH_VOTED, 0x27008, NULL, 0);
UFS_BRANCH(GccQup0S5Clk, "gcc_qupv3_wrap0_s5_clk", 0x27610, 0x62008,
           BIT(15), CLOCK_NODE_BRANCH_VOTED, 0, mQupS5BranchParents, ARRAY_SIZE(mQupS5BranchParents));
STATIC ClockNode *mQup0S1Parents[]={&GccQup0S1Src};
STATIC ClockNode *mQup0S3Parents[]={&GccQup0S3Src};
STATIC ClockNode *mQup0S7Parents[]={&GccGpll0Fixed,&GccQup0S7Src};
STATIC ClockNode *mQup1S0Parents[]={&GccQup1S0Src};
STATIC ClockNode *mQup1S2Parents[]={&GccQup1S2Src};
STATIC ClockNode *mQup1S5Parents[]={&GccQup1S5Src};
UFS_BRANCH(GccQup0S1, "gcc_qupv3_wrap0_s1_clk",0x27140,0x62008,
           BIT(11),CLOCK_NODE_BRANCH_VOTED,0,mQup0S1Parents,ARRAY_SIZE(mQup0S1Parents));
UFS_BRANCH(GccQup0S3, "gcc_qupv3_wrap0_s3_clk",0x273a8,0x62008,
           BIT(13),CLOCK_NODE_BRANCH_VOTED,0,mQup0S3Parents,ARRAY_SIZE(mQup0S3Parents));
/* Linux gcc-sm8450 S7 functional vote/RCG. The debug mux alias is only
 * observation metadata and never owns this CBCR or vote-register bit. */
UFS_BRANCH(GccQup0S7, "gcc_qupv3_wrap0_s7_clk",0x27878,0x62008,
           BIT(17),CLOCK_NODE_BRANCH_VOTED,0,mQup0S7Parents,ARRAY_SIZE(mQup0S7Parents));
UFS_BRANCH(GccQup1S0, "gcc_qupv3_wrap1_s0_clk",0x2800c,0x62008,
           BIT(22),CLOCK_NODE_BRANCH_VOTED,0,mQup1S0Parents,ARRAY_SIZE(mQup1S0Parents));
UFS_BRANCH(GccQup1S2, "gcc_qupv3_wrap1_s2_clk",0x28274,0x62008,
           BIT(24),CLOCK_NODE_BRANCH_VOTED,0,mQup1S2Parents,ARRAY_SIZE(mQup1S2Parents));
UFS_BRANCH(GccQup1S5, "gcc_qupv3_wrap1_s5_clk",0x28610,0x62008,
           BIT(27),CLOCK_NODE_BRANCH_VOTED,0,mQup1S5Parents,ARRAY_SIZE(mQup1S5Parents));
UFS_BRANCH(GccUfsClkref, "gcc_ufs_0_clkref_en", 0x9c000, 0x9c000,
           BIT(0), CLOCK_NODE_BRANCH_HALT, 0, NULL, 0);
UFS_BRANCH(GccUfsPhyAhb, "gcc_ufs_phy_ahb_clk", 0x87020, 0x87020,
           BIT(0), CLOCK_NODE_BRANCH_VOTED, 0x87020, NULL, 0);
UFS_BRANCH(GccUfsPhyAxi, "gcc_ufs_phy_axi_clk", 0x87018, 0x87018,
           BIT(0), CLOCK_NODE_BRANCH_VOTED, 0x87018, mUfsAxiBranchParents, ARRAY_SIZE(mUfsAxiBranchParents));
UFS_BRANCH(GccAggreUfsPhyAxi, "gcc_aggre_ufs_phy_axi_clk", 0x870d4, 0x870d4,
           BIT(0), CLOCK_NODE_BRANCH_VOTED, 0x870d4, NULL, 0);
UFS_BRANCH(GccUfsPhyAux, "gcc_ufs_phy_phy_aux_clk", 0x870a4, 0x870a4,
           BIT(0), CLOCK_NODE_BRANCH_VOTED, 0x870a4, mUfsAuxBranchParents, ARRAY_SIZE(mUfsAuxBranchParents));
UFS_BRANCH(GccUfsRx0, "gcc_ufs_phy_rx_symbol_0_clk", 0x87028, 0x87028,
           BIT(0), CLOCK_NODE_BRANCH_HALT_DELAY, 0, NULL, 0);
UFS_BRANCH(GccUfsRx1, "gcc_ufs_phy_rx_symbol_1_clk", 0x870c0, 0x870c0,
           BIT(0), CLOCK_NODE_BRANCH_HALT_DELAY, 0, NULL, 0);
UFS_BRANCH(GccUfsTx0, "gcc_ufs_phy_tx_symbol_0_clk", 0x87024, 0x87024,
           BIT(0), CLOCK_NODE_BRANCH_HALT_DELAY, 0, NULL, 0);
UFS_BRANCH(GccUfsUnipro, "gcc_ufs_phy_unipro_core_clk", 0x87064, 0x87064,
           BIT(0), CLOCK_NODE_BRANCH_VOTED, 0x87064, mUfsUniproBranchParents, ARRAY_SIZE(mUfsUniproBranchParents));



/* Linux gcc-sm8450.c USB1 core and combo-PHY clocks. */
STATIC ClockRcgFreqTable mUsbMasterFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN,
                                    1, 200000000, 3, 0, 0),
};
#define USB_RCG(Node, NodeName, Cmd, Width, Table) \
  STATIC ClockNode Node = { \
    .Name = NodeName, .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, \
    .CmdRegister = Cmd, .MNDWidth = Width, .HIDWidth = 5, .HwClockCtrl = TRUE, \
    .FreqCount = ARRAY_SIZE(Table), .FreqTable = Table, \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC], \
  }
USB_RCG(GccUsbMasterSrc, "gcc_usb30_prim_master_clk_src", 0x49028, 8, mUsbMasterFreq);
USB_RCG(GccUsbMockSrc, "gcc_usb30_prim_mock_utmi_clk_src", 0x49040, 0, mPcieAuxFreq);
USB_RCG(GccUsbAuxSrc, "gcc_usb3_prim_phy_aux_clk_src", 0x4906c, 0, mPcieAuxFreq);
STATIC ClockNode GccUsbPipeSrc = {
  .Name = "gcc_usb3_prim_phy_pipe_clk_src", .Type = CLOCK_NODE_TYPE_PHY_MUX,
  .MuxRegister = 0x49068, .MuxMask = GEN_MSK(1, 0),
  .MuxPhyValue = 0, .MuxRefValue = 2,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode *mUsbMasterParents[] = { &GccUsbMasterSrc };
STATIC ClockNode *mUsbMockParents[] = { &GccUsbMockSrc };
STATIC ClockNode *mUsbAuxParents[] = { &GccUsbAuxSrc };
STATIC ClockNode *mUsbPipeParents[] = { &GccUsbPipeSrc };
#define USB_BRANCH(Node, NodeName, Reg, Flags, Hwcg, ParentList) \
  STATIC ClockNode Node = { \
    .Name = NodeName, .Type = CLOCK_NODE_TYPE_BRANCH_2, \
    .HaltRegister = Reg, .EnableRegister = Reg, .EnableMsk = BIT(0), \
    .HaltCheckFlag = Flags, .HwcgRegister = Hwcg, \
    .HwcgMsk = (Hwcg) != 0 ? BIT(1) : 0, \
    .ParentCount = ARRAY_SIZE(ParentList), .Parents = ParentList, \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC], \
  }
USB_BRANCH(GccUsbMaster, "gcc_usb30_prim_master_clk", 0x49018,
           CLOCK_NODE_BRANCH_HALT, 0, mUsbMasterParents);
USB_BRANCH(GccUsbAggre, "gcc_aggre_usb3_prim_axi_clk", 0x49088,
           CLOCK_NODE_BRANCH_VOTED, 0x49088, mUsbMasterParents);
USB_BRANCH(GccUsbCfg, "gcc_cfg_noc_usb3_prim_axi_clk", 0x49084,
           CLOCK_NODE_BRANCH_VOTED, 0x49084, mUsbMasterParents);
STATIC ClockNode GccUsbMock = {
  .Name="gcc_usb30_prim_mock_utmi_clk",.Type=CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister=0x49024,.EnableRegister=0x49024,.EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=ARRAY_SIZE(mUsbMockParents),.Parents=mUsbMockParents,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
  .DividerRegister=0x49058,.DividerWidth=4,.DividerReadOnly=TRUE,.RequiredDivider=1,
};
USB_BRANCH(GccUsbAux, "gcc_usb3_prim_phy_aux_clk", 0x4905c,
           CLOCK_NODE_BRANCH_HALT, 0, mUsbAuxParents);
USB_BRANCH(GccUsbComAux, "gcc_usb3_prim_phy_com_aux_clk", 0x49060,
           CLOCK_NODE_BRANCH_HALT, 0, mUsbAuxParents);
USB_BRANCH(GccUsbPipe, "gcc_usb3_prim_phy_pipe_clk", 0x49064,
           CLOCK_NODE_BRANCH_HALT_DELAY, 0x49064, mUsbPipeParents);
UFS_BRANCH(GccUsbSleep, "gcc_usb30_prim_sleep_clk", 0x49020, 0x49020,
           BIT(0), CLOCK_NODE_BRANCH_HALT, 0, NULL, 0);
UFS_BRANCH(GccUsbRef, "gcc_usb3_0_clkref_en", 0x9c010, 0x9c010,
           BIT(0), CLOCK_NODE_BRANCH_HALT, 0, NULL, 0);
STATIC ClockNode GccUsbGdsc = {
  .Name = "usb30_prim_gdsc", .GdscRegister = 0x49004,
  .PwrStsFlag = CLOCK_NODE_GDSC_PWR_STS_OFF_ON,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccUfsPhyIceCoreClk = {
  .Name             = "gcc_ufs_phy_ice_core_clk",
  .Type             = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister     = 0x8706c,
  .EnableRegister   = 0x8706c,
  .EnableMsk        = BIT(0),
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
  .ParentCount      = ARRAY_SIZE (mUfsIceBranchParents),
  .Parents          = mUfsIceBranchParents,
};

STATIC ClockNode GccUfsPhyGdsc = {
  .Name             = "ufs_phy_gdsc",
  .GdscRegister     = 0x87004,
  .PwrStsFlag       = CLOCK_NODE_GDSC_PWR_STS_OFF_ON,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie0Gdsc = {
  .Name             = "pcie_0_gdsc",
  .GdscRegister     = 0x7b004,
  .PwrStsFlag       = CLOCK_NODE_GDSC_PWR_STS_RET_ON,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie1Gdsc = {
  .Name             = "pcie_1_gdsc",
  .GdscRegister     = 0x9d004,
  .PwrStsFlag       = CLOCK_NODE_GDSC_PWR_STS_RET_ON,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

/* RCG and PHY-mux nodes. The host adapter handles the PHY mux's 0/2
 * selection values because ClockNode's generic branch ABI only carries a
 * single enable mask. */
STATIC ClockNode GccPcie0AuxClkSrc = {
  .Name             = "gcc_pcie_0_aux_clk_src",
  .Type             = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister      = 0x7b064,
  .MNDWidth         = 16,
  .HIDWidth         = 5,
  .HwClockCtrl      = TRUE,
  .FreqCount        = ARRAY_SIZE(mPcieAuxFreq),
  .FreqTable        = mPcieAuxFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie0PhyRchngClkSrc = {
  .Name             = "gcc_pcie_0_phy_rchng_clk_src",
  .Type             = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister      = 0x7b048,
  .HIDWidth         = 5,
  .HwClockCtrl      = TRUE,
  .FreqCount        = ARRAY_SIZE(mPcieRchngFreq),
  .FreqTable        = mPcieRchngFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie0PipeClkSrc = {
  .Name             = "gcc_pcie_0_pipe_clk_src",
  .Type             = CLOCK_NODE_TYPE_PHY_MUX,
  .MuxRegister      = 0x7b060,
  .MuxMask          = GEN_MSK(1, 0),
  .MuxPhyValue      = 0,
  .MuxRefValue      = 2,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie1AuxClkSrc = {
  .Name             = "gcc_pcie_1_aux_clk_src",
  .Type             = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister      = 0x9d068,
  .MNDWidth         = 16,
  .HIDWidth         = 5,
  .HwClockCtrl      = TRUE,
  .FreqCount        = ARRAY_SIZE(mPcieAuxFreq),
  .FreqTable        = mPcieAuxFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie1PhyAuxClkSrc = {
  .Name             = "gcc_pcie_1_phy_aux_clk_src",
  /* Linux models this as a regular clk_regmap_mux.  The QMP PHY registers a
   * fixed 20 MHz parent; selecting it is a parent operation, not a gate. */
  .Type             = CLOCK_NODE_TYPE_MUX,
  .MuxRegister      = 0x9d080,
  .MuxMask          = GEN_MSK(1, 0),
  .MuxPhyValue      = 0,
  .MuxRefValue      = 2,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie1PhyRchngClkSrc = {
  .Name             = "gcc_pcie_1_phy_rchng_clk_src",
  .Type             = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister      = 0x9d04c,
  .HIDWidth         = 5,
  .HwClockCtrl      = TRUE,
  .FreqCount        = ARRAY_SIZE(mPcieRchngFreq),
  .FreqTable        = mPcieRchngFreq,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

STATIC ClockNode GccPcie1PipeClkSrc = {
  .Name             = "gcc_pcie_1_pipe_clk_src",
  .Type             = CLOCK_NODE_TYPE_PHY_MUX,
  .MuxRegister      = 0x9d064,
  .MuxMask          = GEN_MSK(1, 0),
  .MuxPhyValue      = 0,
  .MuxRefValue      = 2,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],
};

/* Mirror the parent relationships in Linux's common-clock framework.  The
 * PCIe host requests the leaf branch clocks; these links make a cold enable
 * program and ungate the corresponding source before the branch gate. */
STATIC ClockNode *mPcie0AuxParents[] = { &GccPcie0AuxClkSrc };
STATIC ClockNode *mPcie0RchngParents[] = { &GccPcie0PhyRchngClkSrc };
STATIC ClockNode *mPcie1AuxParents[] = { &GccPcie1AuxClkSrc };
STATIC ClockNode *mPcie0PipeParents[] = { &GccPcie0PipeClkSrc };
STATIC ClockNode *mPcie1PipeParents[] = { &GccPcie1PipeClkSrc };
STATIC ClockNode *mPcie1PhyAuxParents[] = { &GccPcie1PhyAuxClkSrc };
STATIC ClockNode *mPcie1RchngParents[] = { &GccPcie1PhyRchngClkSrc };

/* SM8450 display clock controller (Linux dispcc-sm8450.c). DSI0/1,
 * DP and MDP resources consumed by the qualified MDSS owners. The source
 * nodes are real RCG2 nodes so the native provider can select the DSI PHY
 * parent while the branch remains gated. */
STATIC ClockRcgFreqTable mDispSimpleFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_BI_TCXO, 0, 19200000, 1, 0, 0),
};

STATIC ClockRcgFreqTable mDispMdpFreq[] = {
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_BI_TCXO, 0, 19200000, 1, 0, 0),
  /* Linux's disp_cc_mdss_mdp_clk_src table.  The parent is PLL0 and
   * CLK_SET_RATE_PARENT programs PLL0 to three times this leaf rate. */
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 85714286, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 100000000, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 150000000, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 172000000, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 200000000, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 325000000, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 375000000, 3, 0, 0),
  CLOCK_NODE_RCG_FREQ_TABLE_ELEMENT(
      SM8450_CLOCK_RCG2_SRC_DISP_PLL0, 1, 500000000, 3, 0, 0),
};

#define DISP_RCG(Node, NodeName, Cmd, Mnd, Hid, Freqs) \
  STATIC ClockNode Node = { \
    .Name = NodeName, .Type = CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, \
    .CmdRegister = Cmd, .MNDWidth = Mnd, .HIDWidth = Hid, \
    .HwClockCtrl = FALSE, .FreqCount = ARRAY_SIZE(Freqs), \
    .FreqTable = Freqs, \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC], \
  }

DISP_RCG(DispMdssAhbSrc, "disp_cc_mdss_ahb_clk_src", 0x8324, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssByte0Src, "disp_cc_mdss_byte0_clk_src", 0x8134, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssByte1Src, "disp_cc_mdss_byte1_clk_src", 0x8150, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssEsc0Src, "disp_cc_mdss_esc0_clk_src", 0x816c, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssEsc1Src, "disp_cc_mdss_esc1_clk_src", 0x8184, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssMdpSrc, "disp_cc_mdss_mdp_clk_src", 0x80ec, 0, 5,
         mDispMdpFreq);
DISP_RCG(DispMdssPclk0Src, "disp_cc_mdss_pclk0_clk_src", 0x80bc, 8, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssPclk1Src, "disp_cc_mdss_pclk1_clk_src", 0x80d4, 8, 5,
         mDispSimpleFreq);
DISP_RCG(DispMdssVsyncSrc, "disp_cc_mdss_vsync_clk_src", 0x811c, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispDpAuxSrc, "disp_cc_mdss_dptx0_aux_clk_src", 0x81ec, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispDpLinkSrc, "disp_cc_mdss_dptx0_link_clk_src", 0x819c, 0, 5,
         mDispSimpleFreq);
DISP_RCG(DispDpPixelSrc, "disp_cc_mdss_dptx0_pixel0_clk_src", 0x81bc, 16, 5,
         mDispSimpleFreq);
STATIC ClockNode *mDpAuxParents[] = { &DispDpAuxSrc };
STATIC ClockNode *mDpLinkParents[] = { &DispDpLinkSrc };
STATIC ClockNode *mDpPixelParents[] = { &DispDpPixelSrc };

STATIC ClockNode *mDispAhbParents[] = { &DispMdssAhbSrc };
STATIC ClockNode *mDispByte0Parents[] = { &DispMdssByte0Src };
STATIC ClockNode *mDispByte1Parents[] = { &DispMdssByte1Src };
STATIC ClockNode *mDispEsc0Parents[] = { &DispMdssEsc0Src };
STATIC ClockNode *mDispEsc1Parents[] = { &DispMdssEsc1Src };
STATIC ClockNode *mDispMdpParents[] = { &DispMdssMdpSrc };
STATIC ClockNode *mDispPclk0Parents[] = { &DispMdssPclk0Src };
STATIC ClockNode *mDispPclk1Parents[] = { &DispMdssPclk1Src };
STATIC ClockNode *mDispVsyncParents[] = { &DispMdssVsyncSrc };

#define DISP_BRANCH_PARENT(Node, NodeName, Halt, Enable, Flags, ParentList) \
  STATIC ClockNode Node = { \
    .Name = NodeName, .Type = CLOCK_NODE_TYPE_BRANCH_2, \
    .HaltRegister = Halt, .HaltCheckFlag = Flags, \
    .EnableRegister = Enable, .EnableMsk = BIT(0), \
    .ParentCount = ARRAY_SIZE(ParentList), .Parents = ParentList, \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC], \
  }

DISP_BRANCH_PARENT(DispMdssAhb, "disp_cc_mdss_ahb_clk", 0x80a4, 0x80a4,
                  CLOCK_NODE_BRANCH_HALT, mDispAhbParents);
DISP_BRANCH_PARENT(DispMdssNonGdscAhb, "disp_cc_mdss_non_gdsc_ahb_clk",
                  0xc004, 0xc004, CLOCK_NODE_BRANCH_VOTED,
                  mDispAhbParents);
DISP_BRANCH_PARENT(DispMdssMdp, "disp_cc_mdss_mdp_clk", 0x800c, 0x800c,
                  CLOCK_NODE_BRANCH_HALT, mDispMdpParents);
DISP_BRANCH_PARENT(DispMdssMdpLut, "disp_cc_mdss_mdp_lut_clk", 0x801c, 0x801c,
                  CLOCK_NODE_BRANCH_VOTED, mDispMdpParents);
DISP_BRANCH_PARENT(DispMdssVsync, "disp_cc_mdss_vsync_clk", 0x8024, 0x8024,
                  CLOCK_NODE_BRANCH_HALT, mDispVsyncParents);
DISP_BRANCH_PARENT(DispMdssEsc0, "disp_cc_mdss_esc0_clk", 0x8038, 0x8038,
                  CLOCK_NODE_BRANCH_HALT, mDispEsc0Parents);
DISP_BRANCH_PARENT(DispMdssEsc1, "disp_cc_mdss_esc1_clk", 0x803c, 0x803c,
                  CLOCK_NODE_BRANCH_HALT, mDispEsc1Parents);
DISP_BRANCH_PARENT(DispMdssByte0, "disp_cc_mdss_byte0_clk", 0x8028, 0x8028,
                  CLOCK_NODE_BRANCH_HALT, mDispByte0Parents);
DISP_BRANCH_PARENT(DispMdssByte1, "disp_cc_mdss_byte1_clk", 0x8030, 0x8030,
                  CLOCK_NODE_BRANCH_HALT, mDispByte1Parents);
DISP_BRANCH_PARENT(DispMdssPclk0, "disp_cc_mdss_pclk0_clk", 0x8004, 0x8004,
                  CLOCK_NODE_BRANCH_HALT, mDispPclk0Parents);
DISP_BRANCH_PARENT(DispMdssPclk1, "disp_cc_mdss_pclk1_clk", 0x8008, 0x8008,
                  CLOCK_NODE_BRANCH_HALT, mDispPclk1Parents);
DISP_BRANCH_PARENT(DispDpAux, "disp_cc_mdss_dptx0_aux_clk", 0x8058, 0x8058,
                  CLOCK_NODE_BRANCH_HALT, mDpAuxParents);
DISP_BRANCH_PARENT(DispDpLink, "disp_cc_mdss_dptx0_link_clk", 0x8040, 0x8040,
                  CLOCK_NODE_BRANCH_HALT, mDpLinkParents);
DISP_BRANCH_PARENT(DispDpPixel, "disp_cc_mdss_dptx0_pixel0_clk", 0x8050, 0x8050,
                  CLOCK_NODE_BRANCH_HALT, mDpPixelParents);
/* Link interface has an independent read-only /N divider (0x81b4). */
STATIC ClockNode DispDpLinkIntf = {
  .Name = "disp_cc_mdss_dptx0_link_intf_clk", .Type = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister = 0x8048, .EnableRegister = 0x8048, .EnableMsk = BIT(0),
  .HaltCheckFlag = CLOCK_NODE_BRANCH_HALT,
  .ParentCount = ARRAY_SIZE(mDpLinkParents), .Parents = mDpLinkParents,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC],
  .DividerRegister = 0x81b4, .DividerWidth = 4, .DividerReadOnly = TRUE,
};

/* Linux's byte interface branch is parented by the divider at 0x814c and
 * then byte0 RCG. Collapse that divider into the branch metadata while
 * retaining its source dependency. */
STATIC ClockNode DispMdssByte0Intf = {
  .Name = "disp_cc_mdss_byte0_intf_clk",
  .Type = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister = 0x802c,
  .HaltCheckFlag = CLOCK_NODE_BRANCH_HALT,
  .EnableRegister = 0x802c,
  .EnableMsk = BIT(0),
  .ParentCount = ARRAY_SIZE(mDispByte0Parents), .Parents = mDispByte0Parents,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC],
  .DividerRegister = 0x814c, .DividerWidth = 4, .DividerReadOnly = FALSE,
};
STATIC ClockNode DispMdssByte1Intf = {
  .Name = "disp_cc_mdss_byte1_intf_clk", .Type = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister = 0x8034, .HaltCheckFlag = CLOCK_NODE_BRANCH_HALT,
  .EnableRegister = 0x8034, .EnableMsk = BIT(0),
  .ParentCount = ARRAY_SIZE(mDispByte1Parents), .Parents = mDispByte1Parents,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC],
  .DividerRegister = 0x8168, .DividerWidth = 4, .DividerReadOnly = FALSE,
};

STATIC ClockNode DispMdssCoreGdsc = {
  .Name = "disp_cc_mdss_core_gdsc",
  .Type = CLOCK_NODE_TYPE_GDSC,
  .GdscRegister = 0x9000,
  .PwrStsFlag = CLOCK_NODE_GDSC_PWR_STS_OFF_ON,
  .GdscFlags = CLOCK_NODE_GDSC_FLAG_RETAIN_FF_EN |
               CLOCK_NODE_GDSC_FLAG_HW_CTL,
  .ParentController = &Sm8450ClockControllers[CC_TYPE_DISP_CC],
};

STATIC ClockNode *mDispClocks[] = {
  &DispMdssNonGdscAhb,
  &DispMdssMdp,
  &DispMdssAhb,
  &DispMdssMdpLut,
  &DispMdssVsync,
  &DispMdssEsc0,
  &DispMdssByte0,
  &DispMdssByte0Intf,
  &DispMdssPclk0,
  &DispMdssAhbSrc,
  &DispMdssByte0Src,
  &DispMdssEsc0Src,
  &DispMdssMdpSrc,
  &DispMdssPclk0Src,
  &DispMdssVsyncSrc,
  &DispDpAuxSrc, &DispDpLinkSrc, &DispDpPixelSrc,
  &DispDpAux, &DispDpLink, &DispDpLinkIntf, &DispDpPixel,
  /* Append: preserve every previously exported graph index. */
  &DispMdssByte1, &DispMdssByte1Intf, &DispMdssPclk1, &DispMdssEsc1,
  &DispMdssByte1Src, &DispMdssPclk1Src, &DispMdssEsc1Src,
};

STATIC ClockNode *mDispGdscs[] = { &DispMdssCoreGdsc };

#define PCIE_BRANCH(Node, NodeName, Halt, Enable, Mask, Flags, Hwcg) \
  STATIC ClockNode Node = {                                      \
    .Name             = NodeName,                                \
    .Type             = CLOCK_NODE_TYPE_BRANCH_2,                \
    .HaltRegister     = Halt,                                    \
    .HaltCheckFlag    = Flags,                                   \
    .HwcgRegister     = Hwcg,                                    \
    .HwcgMsk          = (Hwcg) != 0 ? BIT(1) : 0,                \
    .EnableRegister   = Enable,                                  \
    .EnableMsk        = Mask,                                    \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],    \
  }

#define PCIE_BRANCH_PARENT(Node, NodeName, Halt, Enable, Mask, Flags, Hwcg, \
                           ParentList)                            \
  STATIC ClockNode Node = {                                      \
    .Name             = NodeName,                                \
    .Type             = CLOCK_NODE_TYPE_BRANCH_2,                \
    .HaltRegister     = Halt,                                    \
    .HaltCheckFlag    = Flags,                                   \
    .HwcgRegister     = Hwcg,                                    \
    .HwcgMsk          = (Hwcg) != 0 ? BIT(1) : 0,                \
    .EnableRegister   = Enable,                                  \
    .EnableMsk        = Mask,                                    \
    .ParentCount      = ARRAY_SIZE(ParentList),                  \
    .Parents          = ParentList,                              \
    .ParentController = &Sm8450ClockControllers[CC_TYPE_GCC],    \
  }

PCIE_BRANCH(GccAggreNocPcie0AxiClk, "gcc_aggre_noc_pcie_0_axi_clk",
            0x7b08c, 0x62000, BIT(12), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x7b08c);
PCIE_BRANCH(GccAggreNocPcie1AxiClk, "gcc_aggre_noc_pcie_1_axi_clk",
            0x9d098, 0x62000, BIT(11), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x9d098);
PCIE_BRANCH(GccDdrssPcieSfTbuClk, "gcc_ddrss_pcie_sf_tbu_clk",
            0x9d094, 0x62000, BIT(19), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x9d094);

/* GCC display fabric clocks are BRANCH_HALT_SKIP/HWCG clocks in Linux
 * gcc-sm8450.c; MDSS votes these before touching DISP CC. */
PCIE_BRANCH(GccDispAhbClk, "gcc_disp_ahb_clk",
            0x37004, 0x37004, BIT(0), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x37004);
PCIE_BRANCH(GccDispHfAxiClk, "gcc_disp_hf_axi_clk",
            0x3700c, 0x3700c, BIT(0), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x3700c);
PCIE_BRANCH(GccDispSfAxiClk, "gcc_disp_sf_axi_clk",
            0x37014, 0x37014, BIT(0), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x37014);
PCIE_BRANCH(GccDispXoClk, "gcc_disp_xo_clk",
            0x3701c, 0x3701c, BIT(0), CLOCK_NODE_BRANCH_HALT_SKIP,
            0x3701c);

PCIE_BRANCH(GccPcie0CfgAhbClk, "gcc_pcie_0_cfg_ahb_clk",
            0x7b030, 0x62008, BIT(2), CLOCK_NODE_BRANCH_VOTED, 0x7b030);
PCIE_BRANCH(GccPcie0MstrAxiClk, "gcc_pcie_0_mstr_axi_clk",
            0x7b028, 0x62008, BIT(1), CLOCK_NODE_BRANCH_HALT_SKIP, 0);
PCIE_BRANCH(GccPcie0SlvAxiClk, "gcc_pcie_0_slv_axi_clk",
            0x7b020, 0x62008, BIT(0), CLOCK_NODE_BRANCH_VOTED, 0x7b020);
PCIE_BRANCH(GccPcie0SlvQ2aAxiClk, "gcc_pcie_0_slv_q2a_axi_clk",
            0x7b01c, 0x62008, BIT(5), CLOCK_NODE_BRANCH_VOTED, 0);
PCIE_BRANCH(GccPcie0ClkrefEn, "gcc_pcie_0_clkref_en",
            0x9c004, 0x9c004, BIT(0), 0, 0);
PCIE_BRANCH_PARENT(GccPcie0AuxClk, "gcc_pcie_0_aux_clk",
                   0x7b034, 0x62008, BIT(3), CLOCK_NODE_BRANCH_VOTED, 0,
                   mPcie0AuxParents);
PCIE_BRANCH_PARENT(GccPcie0PhyRchngClk, "gcc_pcie_0_phy_rchng_clk",
                   0x7b044, 0x62000, BIT(22), CLOCK_NODE_BRANCH_VOTED, 0,
                   mPcie0RchngParents);
PCIE_BRANCH_PARENT(GccPcie0PipeClk, "gcc_pcie_0_pipe_clk",
                   0x7b03c, 0x62008, BIT(4), CLOCK_NODE_BRANCH_HALT_SKIP, 0,
                   mPcie0PipeParents);

PCIE_BRANCH(GccPcie1CfgAhbClk, "gcc_pcie_1_cfg_ahb_clk",
            0x9d02c, 0x62000, BIT(28), CLOCK_NODE_BRANCH_VOTED, 0x9d02c);
PCIE_BRANCH(GccPcie1MstrAxiClk, "gcc_pcie_1_mstr_axi_clk",
            0x9d024, 0x62000, BIT(27), CLOCK_NODE_BRANCH_HALT_SKIP, 0);
PCIE_BRANCH(GccPcie1SlvAxiClk, "gcc_pcie_1_slv_axi_clk",
            0x9d01c, 0x62000, BIT(26), CLOCK_NODE_BRANCH_VOTED, 0x9d01c);
PCIE_BRANCH(GccPcie1SlvQ2aAxiClk, "gcc_pcie_1_slv_q2a_axi_clk",
            0x9d018, 0x62000, BIT(25), CLOCK_NODE_BRANCH_VOTED, 0);
PCIE_BRANCH(GccPcie1ClkrefEn, "gcc_pcie_1_clkref_en",
            0x9c008, 0x9c008, BIT(0), 0, 0);
PCIE_BRANCH_PARENT(GccPcie1AuxClk, "gcc_pcie_1_aux_clk",
                   0x9d030, 0x62000, BIT(29), CLOCK_NODE_BRANCH_VOTED, 0,
                   mPcie1AuxParents);
PCIE_BRANCH_PARENT(GccPcie1PhyAuxClk, "gcc_pcie_1_phy_aux_clk",
                   0x9d038, 0x62000, BIT(24), CLOCK_NODE_BRANCH_VOTED, 0,
                   mPcie1PhyAuxParents);
PCIE_BRANCH_PARENT(GccPcie1PhyRchngClk, "gcc_pcie_1_phy_rchng_clk",
                   0x9d048, 0x62000, BIT(23), CLOCK_NODE_BRANCH_VOTED, 0,
                   mPcie1RchngParents);
PCIE_BRANCH_PARENT(GccPcie1PipeClk, "gcc_pcie_1_pipe_clk",
                   0x9d040, 0x62000, BIT(30), CLOCK_NODE_BRANCH_HALT_SKIP, 0,
                   mPcie1PipeParents);

#include "Sm8450GpuClock.h"

STATIC ClockNode *mGccClocks[SM8450_CLOCK_GCC_CLOCK_COUNT] = {
  [GCC_GPU_GPLL0_CLK_SRC] = &GccGpuGpllMain,
  [GCC_GPU_GPLL0_DIV_CLK_SRC] = &GccGpuGpllDiv,
  [GCC_DDRSS_GPU_AXI_CLK] = &GccDdrssGpuAxi,
  [GCC_GPU_MEMNOC_GFX_CLK] = &GccGpuMemnoc,
  [GCC_GPU_SNOC_DVM_GFX_CLK] = &GccGpuSnocDvm,
  [GCC_GPLL0] = &GccGpll0Fixed,
  [GCC_SDCC2_AHB_CLK] = &GccSdcc2Ahb,
  [GCC_SDCC2_APPS_CLK] = &GccSdcc2Apps,
  [GCC_SDCC2_APPS_CLK_SRC] = &GccSdcc2AppsSrc,
  [GCC_USB30_PRIM_MASTER_CLK] = &GccUsbMaster,
  [GCC_USB30_PRIM_MASTER_CLK_SRC] = &GccUsbMasterSrc,
  [GCC_AGGRE_USB3_PRIM_AXI_CLK] = &GccUsbAggre,
  [GCC_CFG_NOC_USB3_PRIM_AXI_CLK] = &GccUsbCfg,
  [GCC_USB30_PRIM_MOCK_UTMI_CLK] = &GccUsbMock,
  [GCC_USB30_PRIM_MOCK_UTMI_CLK_SRC] = &GccUsbMockSrc,
  [GCC_USB30_PRIM_SLEEP_CLK] = &GccUsbSleep,
  [GCC_USB3_0_CLKREF_EN] = &GccUsbRef,
  [GCC_USB3_PRIM_PHY_AUX_CLK] = &GccUsbAux,
  [GCC_USB3_PRIM_PHY_AUX_CLK_SRC] = &GccUsbAuxSrc,
  [GCC_USB3_PRIM_PHY_COM_AUX_CLK] = &GccUsbComAux,
  [GCC_USB3_PRIM_PHY_PIPE_CLK] = &GccUsbPipe,
  [GCC_USB3_PRIM_PHY_PIPE_CLK_SRC] = &GccUsbPipeSrc,
  [GCC_AGGRE_NOC_PCIE_0_AXI_CLK] = &GccAggreNocPcie0AxiClk,
  [GCC_AGGRE_NOC_PCIE_1_AXI_CLK] = &GccAggreNocPcie1AxiClk,
  [GCC_AGGRE_UFS_PHY_AXI_CLK]    = &GccAggreUfsPhyAxi,
  [GCC_DDRSS_PCIE_SF_TBU_CLK]    = &GccDdrssPcieSfTbuClk,
  [GCC_DISP_AHB_CLK]             = &GccDispAhbClk,
  [GCC_DISP_HF_AXI_CLK]          = &GccDispHfAxiClk,
  [GCC_DISP_SF_AXI_CLK]          = &GccDispSfAxiClk,
  [GCC_DISP_XO_CLK]              = &GccDispXoClk,
  [GCC_PCIE_0_AUX_CLK]           = &GccPcie0AuxClk,
  [GCC_PCIE_0_AUX_CLK_SRC]       = &GccPcie0AuxClkSrc,
  [GCC_PCIE_0_CFG_AHB_CLK]       = &GccPcie0CfgAhbClk,
  [GCC_PCIE_0_CLKREF_EN]         = &GccPcie0ClkrefEn,
  [GCC_PCIE_0_MSTR_AXI_CLK]      = &GccPcie0MstrAxiClk,
  [GCC_PCIE_0_PHY_RCHNG_CLK]     = &GccPcie0PhyRchngClk,
  [GCC_PCIE_0_PHY_RCHNG_CLK_SRC] = &GccPcie0PhyRchngClkSrc,
  [GCC_PCIE_0_PIPE_CLK]          = &GccPcie0PipeClk,
  [GCC_PCIE_0_PIPE_CLK_SRC]      = &GccPcie0PipeClkSrc,
  [GCC_PCIE_0_SLV_AXI_CLK]       = &GccPcie0SlvAxiClk,
  [GCC_PCIE_0_SLV_Q2A_AXI_CLK]   = &GccPcie0SlvQ2aAxiClk,
  [GCC_PCIE_1_AUX_CLK]           = &GccPcie1AuxClk,
  [GCC_PCIE_1_AUX_CLK_SRC]       = &GccPcie1AuxClkSrc,
  [GCC_PCIE_1_CFG_AHB_CLK]       = &GccPcie1CfgAhbClk,
  [GCC_PCIE_1_CLKREF_EN]         = &GccPcie1ClkrefEn,
  [GCC_PCIE_1_MSTR_AXI_CLK]      = &GccPcie1MstrAxiClk,
  [GCC_PCIE_1_PHY_AUX_CLK]       = &GccPcie1PhyAuxClk,
  [GCC_PCIE_1_PHY_AUX_CLK_SRC]   = &GccPcie1PhyAuxClkSrc,
  [GCC_PCIE_1_PHY_RCHNG_CLK]     = &GccPcie1PhyRchngClk,
  [GCC_PCIE_1_PHY_RCHNG_CLK_SRC] = &GccPcie1PhyRchngClkSrc,
  [GCC_PCIE_1_PIPE_CLK]          = &GccPcie1PipeClk,
  [GCC_PCIE_1_PIPE_CLK_SRC]      = &GccPcie1PipeClkSrc,
  [GCC_PCIE_1_SLV_AXI_CLK]       = &GccPcie1SlvAxiClk,
  [GCC_PCIE_1_SLV_Q2A_AXI_CLK]   = &GccPcie1SlvQ2aAxiClk,
  [GCC_UFS_PHY_ICE_CORE_CLK]     = &GccUfsPhyIceCoreClk,
  [GCC_QUPV3_WRAP0_S1_CLK]       = &GccQup0S1,
  [GCC_QUPV3_WRAP0_S1_CLK_SRC]   = &GccQup0S1Src,
  [GCC_QUPV3_WRAP0_S3_CLK]       = &GccQup0S3,
  [GCC_QUPV3_WRAP0_S3_CLK_SRC]   = &GccQup0S3Src,
  [GCC_QUPV3_WRAP0_S7_CLK]       = &GccQup0S7,
  [GCC_QUPV3_WRAP0_S7_CLK_SRC]   = &GccQup0S7Src,
  [GCC_QUPV3_WRAP1_S0_CLK]       = &GccQup1S0,
  [GCC_QUPV3_WRAP1_S0_CLK_SRC]   = &GccQup1S0Src,
  [GCC_QUPV3_WRAP1_S2_CLK]       = &GccQup1S2,
  [GCC_QUPV3_WRAP1_S2_CLK_SRC]   = &GccQup1S2Src,
  [GCC_QUPV3_WRAP1_S5_CLK]       = &GccQup1S5,
  [GCC_QUPV3_WRAP1_S5_CLK_SRC]   = &GccQup1S5Src,
  [GCC_QUPV3_WRAP1_S1_CLK]       = &GccQup1S1Clk,
  [GCC_QUPV3_WRAP1_S1_CLK_SRC]   = &GccQup1S1ClkSrc,
  [GCC_QUPV3_WRAP_1_M_AHB_CLK]   = &GccQup1MAhbClk,
  [GCC_QUPV3_WRAP_1_S_AHB_CLK]   = &GccQup1SAhbClk,
  [GCC_QUPV3_WRAP0_S5_CLK]       = &GccQup0S5Clk,
  [GCC_QUPV3_WRAP0_S5_CLK_SRC]   = &GccQup0S5ClkSrc,
  [GCC_QUPV3_WRAP_0_M_AHB_CLK]   = &GccQup0MAhbClk,
  [GCC_QUPV3_WRAP_0_S_AHB_CLK]   = &GccQup0SAhbClk,
  [GCC_UFS_0_CLKREF_EN]          = &GccUfsClkref,
  [GCC_UFS_PHY_AHB_CLK]          = &GccUfsPhyAhb,
  [GCC_UFS_PHY_AXI_CLK]          = &GccUfsPhyAxi,
  [GCC_UFS_PHY_AXI_CLK_SRC]      = &GccUfsPhyAxiClkSrc,
  [GCC_UFS_PHY_PHY_AUX_CLK]      = &GccUfsPhyAux,
  [GCC_UFS_PHY_PHY_AUX_CLK_SRC]  = &GccUfsPhyAuxClkSrc,
  [GCC_UFS_PHY_RX_SYMBOL_0_CLK]  = &GccUfsRx0,
  [GCC_UFS_PHY_RX_SYMBOL_1_CLK]  = &GccUfsRx1,
  [GCC_UFS_PHY_TX_SYMBOL_0_CLK]  = &GccUfsTx0,
  [GCC_UFS_PHY_UNIPRO_CORE_CLK]  = &GccUfsUnipro,
  [GCC_UFS_PHY_UNIPRO_CORE_CLK_SRC] = &GccUfsPhyUniproClkSrc,
};

STATIC ClockNode *mGccGdscs[] = {
  &GccPcie0Gdsc,
  &GccPcie1Gdsc,
  &GccUfsPhyGdsc,
  &GccUsbGdsc,
};

STATIC ClockController Sm8450ClockControllers[SM8450_CLOCK_CONTROLLER_COUNT] = {
  [CC_TYPE_GCC] = {
    .Address   = 0x00100000,
    .Size      = 0x001f4200,
    .ClkCount  = SM8450_CLOCK_GCC_CLOCK_COUNT,
    .Clks      = mGccClocks,
    .GdscCount = ARRAY_SIZE(mGccGdscs),
    .Gdscs     = mGccGdscs,
  },
  [CC_TYPE_VIDEO_CC] = { .Address = 0xaaf0000, .Size = 0x10000 },
  [CC_TYPE_CAM_CC]   = { .Address = 0xade0000, .Size = 0x20000 },
  [CC_TYPE_GPU_CC]   = {
    .Address = 0x3d90000, .Size = 0xa000,
    .ClkCount = ARRAY_SIZE(mGpuClocks), .Clks = mGpuClocks,
    .GdscCount = ARRAY_SIZE(mGpuGdscs),
    .Gdscs = mGpuGdscs,
  },
  [CC_TYPE_DISP_CC]  = {
    .Address   = 0x0af00000,
    .Size      = 0x20000,
    .ClkCount  = ARRAY_SIZE (mDispClocks),
    .Clks      = mDispClocks,
    .GdscCount = ARRAY_SIZE (mDispGdscs),
    .Gdscs     = mDispGdscs,
  },
  [CC_TYPE_APSS_CC]  = { .Address = 0x17a80000, .Size = 0x21000 },
  [CC_TYPE_MC_CC]    = { .Address = 0x190ba000, .Size = 0x54 },
};

#ifndef _KERNEL_MODE
STATIC ClockDriverContext Sm8450ClockContext = {
  .ClockControllerCount = SM8450_CLOCK_CONTROLLER_COUNT,
  .ClockControllers     = Sm8450ClockControllers,
  .PrepareSource        = NULL,
};
#endif

/* Linux dispcc-sm8450 probe performs these two controller-level writes before
 * registering any DISPCC clocks.  Without them the MDP RCG update handshake
 * can remain asserted even when the GDSC and PLL parent are enabled: MDP
 * clock gating is disabled and the DISPCC XO branch is not held on. */
#define SM8450_DISP_CC_MISC_CMD_OFFSET  0xF000U
#define SM8450_DISP_CC_XO_CLK_OFFSET    0xE05CU
#define SM8450_DISP_CC_MISC_MDP_GATING  BIT (4)
#define SM8450_DISP_CC_XO_CLK_ENABLE    BIT (0)

/* SM8450 DISPCC PLL0/PLL1 are Lucid EVO alpha PLLs.  Linux's dispcc
 * provider configures these parents before a child RCG selects them; leaving
 * the parent unowned makes the RCG UPDATE bit wait indefinitely.  The native
 * ClockNode ABI intentionally has no analogue-PLL node, so keep this small
 * target hook beside the target register description and call it only for a
 * DISPCC RCG whose selected parent is one of these PLLs. */
#define SM8450_DISP_PLL0_OFFSET       0x0000U
#define SM8450_DISP_PLL1_OFFSET       0x1000U
#define SM8450_DISP_PLL_MODE          0x0000U
#define SM8450_DISP_PLL_OPMODE        0x0004U
#define SM8450_DISP_PLL_L             0x0010U
#define SM8450_DISP_PLL_ALPHA         0x0014U
#define SM8450_DISP_PLL_USER_CTL      0x0018U
#define SM8450_DISP_PLL_USER_CTL_U    0x001CU
#define SM8450_DISP_PLL_CONFIG        0x0020U
#define SM8450_DISP_PLL_CONFIG_U      0x0024U
#define SM8450_DISP_PLL_CONFIG_U1     0x0028U
#define SM8450_DISP_PLL_TEST          0x002CU
#define SM8450_DISP_PLL_TEST_U        0x0030U
#define SM8450_DISP_PLL_TEST_U1       0x0034U
#define SM8450_DISP_PLL_TEST_U2       0x0038U
#define SM8450_DISP_PLL_OUTCTRL       BIT (0)
#define SM8450_DISP_PLL_RESET_N       BIT (2)
#define SM8450_DISP_PLL_PCAL_PENDING  BIT (8)
#define SM8450_DISP_PLL_LOCK_DET      BIT (31)
#define SM8450_DISP_PLL_LATCH_INPUT  BIT (14)
#define SM8450_DISP_PLL_LATCH_ACK    BIT (13)
#define SM8450_DISP_PLL_OUT_MASK      0x7U
#define SM8450_DISP_PLL_RUN           0x1U
#define SM8450_DISP_PLL_STANDBY       0x0U
#define SM8450_DISP_PLL_CAL_L         0x44U
#define SM8450_DISP_PLL_ALPHA_WIDTH   16U
#define SM8450_DISP_PLL_PARENT_HZ     19200000ULL

STATIC BOOLEAN
Sm8450ClockWindowFits (CONST ClockController *Controller, UINTN Offset)
{
  return Controller != NULL && Controller->Address != 0 &&
         (Controller->Address & 3U) == 0 && Controller->Size >= 4 &&
         Offset <= Controller->Size - 4U &&
         Controller->Address <= (UINTN)-1 - Controller->Size;
}

STATIC CR_STATUS
Sm8450DispPllRun (
  IN UINTN Base
  )
{
  UINT32 Index;

  CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                      SM8450_DISP_PLL_RESET_N, SM8450_DISP_PLL_RESET_N);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_OPMODE, SM8450_DISP_PLL_RUN);
  for (Index = 0; Index < 1500; ++Index) {
    if ((CrMmioRead32 (Base + SM8450_DISP_PLL_MODE) &
         SM8450_DISP_PLL_LOCK_DET) != 0) {
      break;
    }
    cr_sleep (1);
  }
  if (Index == 1500) {
    return CR_TIMEOUT;
  }
  CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_USER_CTL,
                      SM8450_DISP_PLL_OUT_MASK,
                      SM8450_DISP_PLL_OUT_MASK);
  CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                      SM8450_DISP_PLL_OUTCTRL,
                      SM8450_DISP_PLL_OUTCTRL);
  CrMemoryFence ();
  return CR_SUCCESS;
}

STATIC VOID
Sm8450DispPllRateConfig (
  IN  UINT64 RateHz,
  OUT UINT32 *L,
  OUT UINT32 *Alpha
  )
{
  UINT64 Quotient;
  UINT64 Remainder;

  Quotient = RateHz / SM8450_DISP_PLL_PARENT_HZ;
  Remainder = RateHz % SM8450_DISP_PLL_PARENT_HZ;
  /* Match Linux alpha_pll_round_rate(): round the fractional alpha up so
   * the selected parent is never below the requested rate. */
  Remainder = (Remainder << SM8450_DISP_PLL_ALPHA_WIDTH) +
              SM8450_DISP_PLL_PARENT_HZ - 1;
  *L = (UINT32)Quotient;
  *Alpha = (UINT32)(Remainder / SM8450_DISP_PLL_PARENT_HZ);
  /* The Linux helper can round the fractional part to exactly one whole
   * alpha.  Carry that into L before writing the 16-bit alpha register. */
  if (*Alpha >= (1U << SM8450_DISP_PLL_ALPHA_WIDTH)) {
    ++(*L);
    *Alpha = 0;
  }
}

STATIC CR_STATUS
Sm8450DispPllSetRate (
  IN UINTN Base,
  IN UINT64 RateHz
  )
{
  UINT32 L;
  UINT32 Alpha;
  UINT32 Mode;
  UINT32 CurrentL;
  UINT32 CurrentAlpha;
  UINT32 Index;

  Sm8450DispPllRateConfig (RateHz, &L, &Alpha);
  CurrentL = CrMmioRead32 (Base + SM8450_DISP_PLL_L) & 0xFFFFU;
  CurrentAlpha = CrMmioRead32 (Base + SM8450_DISP_PLL_ALPHA) & 0xFFFFU;
  if ((CurrentL == L) && (CurrentAlpha == Alpha)) {
    return CR_SUCCESS;
  }

  Mode = CrMmioRead32 (Base + SM8450_DISP_PLL_MODE);
  /* Linux updates Lucid EVO L/alpha in place when the PLL is already
   * running.  Do the same and wait for the MODE lock bit; rewriting the
   * complete configure sequence here would glitch unrelated consumers. */
  if ((Mode & SM8450_DISP_PLL_OUTCTRL) != 0) {
    if (((CrMmioRead32 (Base + SM8450_DISP_PLL_OPMODE) &
          SM8450_DISP_PLL_RUN) == 0) ||
        ((Mode & SM8450_DISP_PLL_LOCK_DET) == 0)) {
      return CR_TIMEOUT;
    }
    CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_L,
                        0xFFFFU, L);
    CrMmioWrite32 (Base + SM8450_DISP_PLL_ALPHA, Alpha);
    CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                        SM8450_DISP_PLL_LATCH_INPUT,
                        SM8450_DISP_PLL_LATCH_INPUT);
    cr_sleep (1);
    if ((CrMmioRead32 (Base + SM8450_DISP_PLL_MODE) &
         SM8450_DISP_PLL_LATCH_ACK) == 0) {
      return CR_TIMEOUT;
    }
    CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                        SM8450_DISP_PLL_LATCH_INPUT, 0);
    for (Index = 0; Index < 1500; ++Index) {
      if ((CrMmioRead32 (Base + SM8450_DISP_PLL_MODE) &
           SM8450_DISP_PLL_LOCK_DET) != 0) {
        return CR_SUCCESS;
      }
      cr_sleep (1);
    }
    return CR_TIMEOUT;
  }

  /* The initial configuration path below is followed by the normal Lucid
   * EVO calibration/run sequence. */
  CrMmioWrite32 (Base + SM8450_DISP_PLL_L,
                 L | (SM8450_DISP_PLL_CAL_L << 16));
  CrMmioWrite32 (Base + SM8450_DISP_PLL_ALPHA, Alpha);
  return CR_SUCCESS;
}

STATIC CR_STATUS
Sm8450DispPllEnable (
  IN UINTN Base,
  IN UINT64 RateHz
  )
{
  UINT32 Mode;
  UINT32 L;
  UINT32 Alpha;

  Mode = CrMmioRead32 (Base + SM8450_DISP_PLL_MODE);
  Sm8450DispPllRateConfig (RateHz, &L, &Alpha);
  if ((Mode & SM8450_DISP_PLL_OUTCTRL) != 0) {
    return Sm8450DispPllSetRate (Base, RateHz);
  }

  /* Match clk_lucid_evo_pll_configure() from Linux.  A running PLL takes
   * the dynamic L/alpha path above; this full configure sequence is only for
   * a stopped parent. */
  CrMmioWrite32 (Base + SM8450_DISP_PLL_L,
                 L | (SM8450_DISP_PLL_CAL_L << 16));
  CrMmioWrite32 (Base + SM8450_DISP_PLL_ALPHA, Alpha);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_CONFIG, 0x20485699U);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_CONFIG_U, 0x00182261U);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_CONFIG_U1, 0x32AA299CU);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_USER_CTL, 0x00000000U);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_USER_CTL_U, 0x00000805U);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_TEST, 0);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_TEST_U, 0);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_TEST_U1, 0);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_TEST_U2, 0);
  CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                      SM8450_DISP_PLL_OUTCTRL, 0);
  CrMmioWrite32 (Base + SM8450_DISP_PLL_OPMODE, SM8450_DISP_PLL_STANDBY);
  CrMemoryFence ();

  /* Linux's Lucid EVO prepare callback performs one calibration run when
   * PCAL_NOT_DONE is set, then resets the PLL before the real enable. */
  Mode = CrMmioRead32 (Base + SM8450_DISP_PLL_MODE);
  if ((Mode & SM8450_DISP_PLL_PCAL_PENDING) != 0) {
    if (CR_ERROR (Sm8450DispPllRun (Base))) {
      return CR_TIMEOUT;
    }
    CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                        SM8450_DISP_PLL_OUTCTRL, 0);
    CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_USER_CTL,
                        SM8450_DISP_PLL_OUT_MASK, 0);
    CrMmioWrite32 (Base + SM8450_DISP_PLL_OPMODE, SM8450_DISP_PLL_STANDBY);
    CrMmioUpdateBits32 (Base + SM8450_DISP_PLL_MODE,
                        SM8450_DISP_PLL_RESET_N, 0);
    CrMemoryFence ();
  }
  return Sm8450DispPllRun (Base);
}

STATIC CR_STATUS
Sm8450ClockPrepareSource (
  IN ClockDriverContext *ClockContext,
  IN ClockNode *TargetClockNode,
  IN CONST ClockRcgFreqTable *TargetFreq
  )
{
  UINTN Base;

  if ((ClockContext == NULL) ||
      (ClockContext->ClockControllers == NULL) ||
      (ClockContext->ClockControllerCount <= CC_TYPE_DISP_CC) ||
      (TargetClockNode == NULL) || (TargetFreq == NULL) ||
      (TargetClockNode->ParentController == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  if (TargetClockNode->ParentController != &ClockContext->ClockControllers[CC_TYPE_DISP_CC]) {
    if (TargetClockNode->ParentController == &ClockContext->ClockControllers[CC_TYPE_GPU_CC]) {
      /* The boot profile uses XO or the fixed, voted 300 MHz GCC output.
       * Branch/RO-divider dependencies acquire that output before this RCG.
       * A direct raw source request must not silently select an unowned PLL. */
      if (TargetFreq->Source.Config == 0) return CR_SUCCESS;
      if (TargetFreq->Source.Config != 6) return CR_UNSUPPORTED;
      ClockController *Gcc = &ClockContext->ClockControllers[CC_TYPE_GCC];
      if (Gcc->Clks == NULL || Gcc->ClkCount <= GCC_GPU_GPLL0_DIV_CLK_SRC)
        return CR_INVALID_PARAMETER;
      ClockNode *Div = Gcc->Clks[GCC_GPU_GPLL0_DIV_CLK_SRC];
      ClockNode *Pll = Gcc->Clks[GCC_GPLL0];
      if (Div == NULL || Div->ReferenceCount == 0 || Div->ReleasePending ||
          Div->ActiveRateHz != 300000000 || !Div->GateHeld ||
          Pll == NULL || Pll->Type != CLOCK_NODE_TYPE_FIXED_PLL ||
          Pll->ReferenceCount == 0 || Pll->ReleasePending)
        return CR_NOT_READY;
      if (!Sm8450ClockWindowFits(Gcc, Div->EnableRegister)) return CR_BAD_BUFFER_SIZE;
      if ((CrMmioRead32(Gcc->Address + Div->EnableRegister) & Div->EnableMsk) == 0)
        return CR_NOT_READY;
      return CrFixedPllCheck(&Pll->FixedPll);
    }
    if (TargetClockNode->ParentController == &ClockContext->ClockControllers[CC_TYPE_GCC] &&
        TargetClockNode->Name != NULL &&
        cr_strcmp(TargetClockNode->Name, "gcc_sdcc2_apps_clk_src") == 0 &&
        TargetFreq->Source.Config != SM8450_CLOCK_RCG2_SRC_BI_TCXO) {
      ClockNode *Pll = ClockContext->ClockControllers[CC_TYPE_GCC].Clks[GCC_GPLL0];
      if (Pll == NULL || Pll->Type != CLOCK_NODE_TYPE_FIXED_PLL ||
          Pll->ReferenceCount == 0) return CR_NOT_READY;
      return CrFixedPllCheck(&Pll->FixedPll);
    }
    return CR_SUCCESS;
  }
  if (!Sm8450ClockWindowFits (TargetClockNode->ParentController,
                              SM8450_DISP_PLL1_OFFSET + SM8450_DISP_PLL_TEST_U2)) {
    return CR_BAD_BUFFER_SIZE;
  }
  UINT64 Divider = (TargetFreq->PreDiv + 1ULL) / 2ULL;
  if (Divider == 0 || TargetFreq->FrequencyHz == 0 ||
      TargetFreq->FrequencyHz > CR_MAX_UINT64 / Divider ||
      TargetFreq->FrequencyHz * Divider > 0xffffULL * SM8450_DISP_PLL_PARENT_HZ) {
    return CR_INVALID_PARAMETER;
  }
  Base = TargetClockNode->ParentController->Address;
  if (TargetFreq->Source.Config == SM8450_CLOCK_RCG2_SRC_DISP_PLL0) {
    /* MDP's /3 parent contract is encoded by PreDiv=5 (hid divider 3). */
    return Sm8450DispPllEnable (
        Base + SM8450_DISP_PLL0_OFFSET,
        TargetFreq->FrequencyHz * ((TargetFreq->PreDiv + 1U) / 2U));
  }
  if ((TargetFreq->Source.Config == 4U) || (TargetFreq->Source.Config == 6U)) {
    return Sm8450DispPllEnable (
        Base + SM8450_DISP_PLL1_OFFSET,
        TargetFreq->FrequencyHz * ((TargetFreq->PreDiv + 1U) / 2U));
  }
  return CR_SUCCESS;
}

#ifndef _KERNEL_MODE
ClockDriverContext *CrSm8450GetNativeClockContext(VOID)
{
  return &Sm8450ClockContext;
}
#endif

CR_STATUS CrSm8450ClockInit(ClockDriverContext *ClockContext)
{
  if (ClockContext == NULL) {
    return CR_INVALID_PARAMETER;
  }

  /* Match Linux's dispcc_sm8450_probe() setup.  These are controller-global
   * DISPCC controls and must be programmed before MDSS requests its first
   * RCG; they are not represented as ordinary ClockNode branches. */
  if (ClockContext->ClockControllers == NULL ||
      ClockContext->ClockControllerCount != CC_TYPE_MAX) {
    return CR_INVALID_PARAMETER;
  }
  ClockController *Gcc = &ClockContext->ClockControllers[CC_TYPE_GCC];
  ClockController *Disp = &ClockContext->ClockControllers[CC_TYPE_DISP_CC];
  if (!Sm8450ClockWindowFits (Gcc, 0x8706cU) ||
      !Sm8450ClockWindowFits (Disp, SM8450_DISP_CC_MISC_CMD_OFFSET) ||
      !Sm8450ClockWindowFits (Disp, SM8450_DISP_CC_XO_CLK_OFFSET)) {
    return CR_BAD_BUFFER_SIZE;
  }
  ClockContext->PrepareSource = Sm8450ClockPrepareSource;
  CrMmioUpdateBits32 (
      ClockContext->ClockControllers[CC_TYPE_DISP_CC].Address +
        SM8450_DISP_CC_MISC_CMD_OFFSET,
      SM8450_DISP_CC_MISC_MDP_GATING,
      SM8450_DISP_CC_MISC_MDP_GATING);
  CrMmioUpdateBits32 (
      ClockContext->ClockControllers[CC_TYPE_DISP_CC].Address +
        SM8450_DISP_CC_XO_CLK_OFFSET,
      SM8450_DISP_CC_XO_CLK_ENABLE,
      SM8450_DISP_CC_XO_CLK_ENABLE);

  /* Preserve the existing UFS ICE force-on vote used by this target. */
  CrMmioUpdateBits32(
      Gcc->Address + 0x8706cU,
      BIT(14), BIT(14));
  return CR_SUCCESS;
}

CR_STATUS
CrSm8450ClockReset(
  IN ClockDriverContext *ClockContext,
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  UINTN Offset;

  if (ClockContext == NULL || ClockContext->ClockControllers == NULL ||
      ClockContext->ClockControllerCount != CC_TYPE_MAX ||
      Controller == NULL || Id == NULL || Assert > 1 ||
      cr_strcmp(Controller, "gcc") != 0) {
    return CR_INVALID_PARAMETER;
  }
  if (cr_strcmp(Id, "gcc_pcie_0_bcr") == 0) {
    Offset = 0x7B000U;
  } else if (cr_strcmp(Id, "gcc_pcie_1_bcr") == 0) {
    Offset = 0x9D000U;
  } else if (cr_strcmp(Id, "gcc_pcie_0_phy_bcr") == 0) {
    Offset = 0x7C01CU;
  } else if (cr_strcmp(Id, "gcc_pcie_1_phy_bcr") == 0) {
    Offset = 0x9E01CU;
  } else if (cr_strcmp(Id, "gcc_sdcc2_bcr") == 0) {
    /* GCC_SDCC2_BCR, Linux gcc-sm8450.c reset table. */
    Offset = 0x24000U;
  } else if (cr_strcmp(Id, "gcc_ufs_phy_bcr") == 0) {
    /* GCC_UFS_PHY_BCR (Linux gcc-sm8450.c, reset ID 25). */
    Offset = 0x87000U;
  } else if (cr_strcmp(Id, "gcc_usb30_prim_bcr") == 0) {
    Offset = 0x49000U;
  } else if (cr_strcmp(Id, "gcc_qusb2phy_prim_bcr") == 0) {
    Offset = 0x22000U;
  } else if (cr_strcmp(Id, "gcc_usb3_dp_phy_prim_bcr") == 0) {
    Offset = 0x60008U;
  } else if (cr_strcmp(Id, "gcc_usb3_phy_prim_bcr") == 0) {
    Offset = 0x60000U;
  } else {
    return CR_UNSUPPORTED;
  }

  ClockController *Gcc = &ClockContext->ClockControllers[CC_TYPE_GCC];
  if (!Sm8450ClockWindowFits (Gcc, Offset)) {
    return CR_BAD_BUFFER_SIZE;
  }
  if (!ClockContext->LockInitialized) {
    return CR_NOT_READY;
  }
  CrLockAcquire (&ClockContext->Lock);
  CrMmioUpdateBits32 (Gcc->Address + Offset, BIT (0), Assert ? BIT (0) : 0U);
  CrMemoryFence ();
  UINT32 Readback = CrMmioRead32 (Gcc->Address + Offset);
  CrLockRelease (&ClockContext->Lock);
  return (Readback & BIT (0)) == (Assert ? BIT (0) : 0U) ? CR_SUCCESS : CR_DEVICE_ERROR;
}

/* Each adapter owns a private mutable graph. The source tables above describe
 * hardware; sharing their vote counters across WDF devices would make removal
 * or a failed PrepareHardware corrupt another instance's ownership. */
#define SM8450_CLOCK_MAX_NODES   256U
#define SM8450_CLOCK_MAX_PARENTS 4U
#define SM8450_CLOCK_MAX_DEPTH   16U

typedef struct {
  ClockDriverContext Clock;
  ClockController Controllers[CC_TYPE_MAX];
  ClockNode *GccClocks[SM8450_CLOCK_GCC_CLOCK_COUNT];
  ClockNode *DispClocks[ARRAY_SIZE (mDispClocks)];
  ClockNode *GpuClocks[ARRAY_SIZE (mGpuClocks)];
  ClockNode *GpuGdscs[ARRAY_SIZE (mGpuGdscs)];
  ClockNode *GccGdscs[ARRAY_SIZE (mGccGdscs)];
  ClockNode *DispGdscs[ARRAY_SIZE (mDispGdscs)];
  ClockNode Nodes[SM8450_CLOCK_MAX_NODES];
  ClockNode *Parents[SM8450_CLOCK_MAX_NODES][SM8450_CLOCK_MAX_PARENTS];
  CONST ClockNode *Originals[SM8450_CLOCK_MAX_NODES];
  UINT8 Visiting[SM8450_CLOCK_MAX_NODES];
  UINTN NodeCount;
} SM8450_CLOCK_STATE;

STATIC CR_STATUS
Sm8450CopyClockNode (SM8450_CLOCK_STATE *State, CONST ClockNode *Original,
                    UINTN Depth, ClockNode **Copy)
{
  UINTN Index;
  UINTN Controller;
  CR_STATUS Status;

  *Copy = NULL;
  if (Original == NULL) {
    return CR_SUCCESS;
  }
  if (Depth > SM8450_CLOCK_MAX_DEPTH ||
      Original->ParentCount > SM8450_CLOCK_MAX_PARENTS ||
      (Original->ParentCount != 0 && Original->Parents == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  for (Index = 0; Index < State->NodeCount; ++Index) {
    if (State->Originals[Index] == Original) {
      if (State->Visiting[Index]) {
        return CR_INVALID_PARAMETER;
      }
      *Copy = &State->Nodes[Index];
      return CR_SUCCESS;
    }
  }
  if (State->NodeCount == SM8450_CLOCK_MAX_NODES) {
    return CR_OUT_OF_RESOURCES;
  }
  for (Controller = 0; Controller < CC_TYPE_MAX; ++Controller) {
    if (Original->ParentController == &Sm8450ClockControllers[Controller]) {
      break;
    }
  }
  if (Controller == CC_TYPE_MAX) {
    return CR_INVALID_PARAMETER;
  }
  Index = State->NodeCount++;
  State->Originals[Index] = Original;
  State->Visiting[Index] = TRUE;
  State->Nodes[Index] = *Original;
  ClockNode *Node = &State->Nodes[Index];
  Node->ParentController = &State->Controllers[Controller];
  if (Node->Type == CLOCK_NODE_TYPE_FIXED_PLL) {
    /* Mutable native template state never becomes an independent lease. */
    Node->FixedPll.Initialized = FALSE;
    Node->FixedPll.Owned = FALSE;
    Node->FixedPll.Ready = FALSE;
    Node->FixedPll.RestoreVote = FALSE;
    Node->FixedPll.Description.Base = 0;
    Node->FixedPll.Description.Size = 0;
  }
  Node->ReferenceCount = 0;
  Node->RestoreControlEnabled = FALSE;
  Node->RestoreHwControlled = FALSE;
  Node->ReleasePending = FALSE;
  Node->RcgRestorePending = FALSE;
  cr_memset(Node->RcgSavedValue,0,sizeof(Node->RcgSavedValue));
  cr_memset(Node->RcgSavedMask,0,sizeof(Node->RcgSavedMask));
  Node->ParentsHeld = 0;
  Node->GateHeld = FALSE;
  Node->ExternalConfigPending = FALSE;
  Node->ActiveRateHz = 0;
  Node->ActiveDivider = 0;
  Node->Parents = Original->ParentCount != 0 ? State->Parents[Index] : NULL;
  for (UINTN Parent = 0; Parent < Original->ParentCount; ++Parent) {
    Status = Sm8450CopyClockNode (State, Original->Parents[Parent], Depth + 1,
                                &Node->Parents[Parent]);
    if (CR_ERROR (Status) || Node->Parents[Parent] == NULL) {
      return CR_ERROR (Status) ? Status : CR_INVALID_PARAMETER;
    }
  }
  State->Visiting[Index] = FALSE;
  *Copy = Node;
  return CR_SUCCESS;
}

UINTN CrSm8450ClockStateSize (VOID)
{
  return sizeof (SM8450_CLOCK_STATE);
}

CR_STATUS
CrSm8450ClockCreate (VOID *Storage, UINTN Size, ClockDriverContext **Context)
{
  CR_STATUS Status;

  if (Context == NULL) {
    return CR_INVALID_PARAMETER;
  }
  *Context = NULL;
  if (Storage == NULL || ((UINTN)Storage & (sizeof (VOID *) - 1U)) != 0 ||
      Size < sizeof (SM8450_CLOCK_STATE)) {
    return CR_INVALID_PARAMETER;
  }
  SM8450_CLOCK_STATE *State = Storage;
  cr_memset (State, 0, sizeof (*State));
  State->Clock.ClockControllerCount = CC_TYPE_MAX;
  State->Clock.ClockControllers = State->Controllers;
  State->Clock.TargetState = State;
  for (UINTN Controller = 0; Controller < CC_TYPE_MAX; ++Controller) {
    State->Controllers[Controller] = Sm8450ClockControllers[Controller];
    /* A newly created instance has no register mapping. */
    State->Controllers[Controller].Address = 0;
    State->Controllers[Controller].MemMapped = FALSE;
    State->Controllers[Controller].Clks = NULL;
    State->Controllers[Controller].Gdscs = NULL;
  }
  State->Controllers[CC_TYPE_GCC].Clks = State->GccClocks;
  State->Controllers[CC_TYPE_GCC].Gdscs = State->GccGdscs;
  State->Controllers[CC_TYPE_DISP_CC].Clks = State->DispClocks;
  State->Controllers[CC_TYPE_DISP_CC].Gdscs = State->DispGdscs;
  State->Controllers[CC_TYPE_GPU_CC].Clks = State->GpuClocks;
  State->Controllers[CC_TYPE_GPU_CC].Gdscs = State->GpuGdscs;
  for (UINTN Controller = 0; Controller < CC_TYPE_MAX; ++Controller) {
    ClockController *Source = &Sm8450ClockControllers[Controller];
    ClockController *Target = &State->Controllers[Controller];
    for (UINTN Index = 0; Index < Source->ClkCount; ++Index) {
      Status = Sm8450CopyClockNode (State, Source->Clks[Index], 0,
                                  &Target->Clks[Index]);
      if (CR_ERROR (Status)) {
        return Status;
      }
    }
    for (UINTN Index = 0; Index < Source->GdscCount; ++Index) {
      Status = Sm8450CopyClockNode (State, Source->Gdscs[Index], 0,
                                  &Target->Gdscs[Index]);
      if (CR_ERROR (Status)) {
        return Status;
      }
    }
  }
  *Context = &State->Clock;
  return CR_SUCCESS;
}

CR_STATUS
CrSm8450ClockBind (ClockDriverContext *Context,
                   CONST CR_CLOCK_MAPPING *Mappings, UINTN Count)
{
  if (Context == NULL || Mappings == NULL || Count != CC_TYPE_MAX ||
      Context->ClockControllerCount != CC_TYPE_MAX ||
      Context->ClockControllers == NULL || Context->LockInitialized) {
    return CR_INVALID_PARAMETER;
  }
  /* Complete preflight: rejection never publishes a partial set of mappings. */
  for (UINTN Index = 0; Index < Count; ++Index) {
    if (Mappings[Index].Address == 0 || (Mappings[Index].Address & 3U) != 0 ||
        Mappings[Index].Size < Sm8450ClockControllers[Index].Size ||
        Mappings[Index].Address > (UINTN)-1 - Mappings[Index].Size) {
      return CR_BAD_BUFFER_SIZE;
    }
  }
  for (UINTN Index = 0; Index < Count; ++Index) {
    Context->ClockControllers[Index].Address = Mappings[Index].Address;
    Context->ClockControllers[Index].Size = Mappings[Index].Size;
    Context->ClockControllers[Index].MemMapped = TRUE;
  }
  return CR_SUCCESS;
}

CR_STATUS
CrSm8450ClockResource (ClockControllerType Type, UINT64 *PhysicalAddress,
                       UINT32 *Size)
{
  if ((UINT32)Type >= CC_TYPE_MAX || PhysicalAddress == NULL || Size == NULL) {
    return CR_INVALID_PARAMETER;
  }
  *PhysicalAddress = Sm8450ClockControllers[Type].Address;
  *Size = Sm8450ClockControllers[Type].Size;
  return CR_SUCCESS;
}


CR_STATUS CrSm8450ClockRelease (ClockDriverContext *Context)
{
  if (Context == NULL || Context->ClockControllerCount != CC_TYPE_MAX ||
      !Context->LockInitialized || Context->TargetState != Context)
    return CR_INVALID_PARAMETER;
  SM8450_CLOCK_STATE *State = (SM8450_CLOCK_STATE *)Context;
  if (Context->ClockControllers != State->Controllers ||
      State->NodeCount > SM8450_CLOCK_MAX_NODES) return CR_INVALID_PARAMETER;
  for (UINTN Index=0;Index<State->NodeCount;++Index) {
    ClockNode *Node=&State->Nodes[Index];
    if (Node->ReleasePending && Node->Type==CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2) {
      CR_STATUS Status=ClockRecoverConfiguration(Context,Node);
      if (CR_ERROR(Status)) return Status;
    }
  }
  /* Caller has drained all users. Release leaves before shared sources. Each
   * leaf acquires its parents once, regardless of its direct reference count. */
  for (;;) {
    ClockNode *Leaf = NULL;
    BOOLEAN Active = FALSE;
    for (UINTN Index = 0; Index < State->NodeCount; ++Index) {
      ClockNode *Node = &State->Nodes[Index];
      if (Node->ReferenceCount == 0 || Node->Type == CLOCK_NODE_TYPE_GDSC) continue;
      Active = TRUE;
      BOOLEAN HasChild = FALSE;
      for (UINTN Child = 0; Child < State->NodeCount && !HasChild; ++Child) {
        ClockNode *Candidate = &State->Nodes[Child];
        if (Candidate->ReferenceCount == 0) continue;
        for (UINTN Parent = 0; Parent < Candidate->ParentCount; ++Parent)
          if (Candidate->Parents[Parent] == Node) { HasChild = TRUE; break; }
      }
      if (!HasChild) { Leaf = Node; break; }
    }
    if (!Active) break;
    if (Leaf == NULL) return CR_INVALID_PARAMETER;
    Leaf->ReferenceCount = 1;
    CR_STATUS Status = ClockEnable (Context, Leaf, Leaf->ActiveRateHz, FALSE);
    if (CR_ERROR (Status)) return Status;
  }
  for (UINTN Index = 0; Index < State->NodeCount; ++Index) {
    ClockNode *Node = &State->Nodes[Index];
    if (Node->Type == CLOCK_NODE_TYPE_GDSC && Node->ReferenceCount != 0) {
      Node->ReferenceCount = 1;
      CR_STATUS Status = ClockGdscSet (Context, Node, FALSE);
      if (CR_ERROR (Status)) return Status;
    }
  }
  return CR_SUCCESS;
}
