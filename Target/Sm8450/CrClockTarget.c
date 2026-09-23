/**
 * SM8450 GCC clock and GDSC target data used by the PCIe host adapter.
 *
 * The Linux GCC provider exposes a sparse clock array (the PCIe clocks are
 * IDs 47..70 and the UFS clocks are IDs 154..156). Keep the same bounded
 * indexing here so GetClockNode() never walks beyond a short compound array.
 * Resource ownership remains in ClockLib; this file only describes registers.
 */

#include <Library/CrTargetClockLib.h>
#include <oskal/common.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_string.h>

#include <Library/BaseLib.h>

#define SM8450_CLOCK_CONTROLLER_COUNT  CC_TYPE_MAX
#define SM8450_CLOCK_GCC_CLOCK_COUNT   157U

enum Sm8450ClockRcg2Sources {
  SM8450_CLOCK_RCG2_SRC_BI_TCXO       = 0,
  /* These values are the GCC parent-map mux values, not DT indices. */
  SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN    = 1,
  SM8450_CLOCK_RCG2_SRC_SLEEP_CLK     = 5,
};

/* IDs from linux/include/dt-bindings/clock/qcom,gcc-sm8450.h. */
enum {
  GCC_AGGRE_NOC_PCIE_0_AXI_CLK = 8,
  GCC_AGGRE_NOC_PCIE_1_AXI_CLK = 9,
  GCC_DDRSS_PCIE_SF_TBU_CLK    = 26,

  GCC_PCIE_0_AUX_CLK           = 47,
  GCC_PCIE_0_AUX_CLK_SRC       = 48,
  GCC_PCIE_0_CFG_AHB_CLK       = 49,
  GCC_PCIE_0_CLKREF_EN         = 50,
  GCC_PCIE_0_MSTR_AXI_CLK      = 51,
  GCC_PCIE_0_PHY_RCHNG_CLK     = 52,
  GCC_PCIE_0_PHY_RCHNG_CLK_SRC = 53,
  GCC_PCIE_0_PIPE_CLK          = 54,
  GCC_PCIE_0_PIPE_CLK_SRC      = 55,
  GCC_PCIE_0_SLV_AXI_CLK       = 56,
  GCC_PCIE_0_SLV_Q2A_AXI_CLK   = 57,
  GCC_PCIE_1_AUX_CLK           = 58,
  GCC_PCIE_1_AUX_CLK_SRC       = 59,
  GCC_PCIE_1_CFG_AHB_CLK       = 60,
  GCC_PCIE_1_CLKREF_EN         = 61,
  GCC_PCIE_1_MSTR_AXI_CLK      = 62,
  GCC_PCIE_1_PHY_AUX_CLK       = 63,
  GCC_PCIE_1_PHY_AUX_CLK_SRC   = 64,
  GCC_PCIE_1_PHY_RCHNG_CLK     = 65,
  GCC_PCIE_1_PHY_RCHNG_CLK_SRC = 66,
  GCC_PCIE_1_PIPE_CLK          = 67,
  GCC_PCIE_1_PIPE_CLK_SRC      = 68,
  GCC_PCIE_1_SLV_AXI_CLK       = 69,
  GCC_PCIE_1_SLV_Q2A_AXI_CLK   = 70,

  GCC_UFS_PHY_ICE_CORE_CLK        = 154,
  GCC_UFS_PHY_ICE_CORE_CLK_SRC    = 155,
  GCC_UFS_PHY_ICE_CORE_HW_CTL_CLK = 156,
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
      SM8450_CLOCK_RCG2_SRC_GPLL0_MAIN, 0, 100000000, 6, 0, 0),
};

STATIC ClockNode GccUfsPhyIceCoreClk = {
  .Name             = "gcc_ufs_phy_ice_core_clk",
  .Type             = CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister     = 0x8706c,
  .EnableRegister   = 0x8706c,
  .EnableMsk        = BIT(0),
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
  /* Linux models this as a two-way mux: value 0 selects the fixed 20 MHz
   * clock exported by the QMP PHY and value 2 selects XO. */
  .Type             = CLOCK_NODE_TYPE_PHY_MUX,
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
STATIC ClockNode *mPcie1PhyAuxParents[] = { &GccPcie1PhyAuxClkSrc };
STATIC ClockNode *mPcie1RchngParents[] = { &GccPcie1PhyRchngClkSrc };

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
            0x7b08c, 0x62000, BIT(12), 0, 0x7b08c);
PCIE_BRANCH(GccAggreNocPcie1AxiClk, "gcc_aggre_noc_pcie_1_axi_clk",
            0x9d098, 0x62000, BIT(11), 0, 0x9d098);
PCIE_BRANCH(GccDdrssPcieSfTbuClk, "gcc_ddrss_pcie_sf_tbu_clk",
            0x9d094, 0x62000, BIT(19), 0, 0x9d094);

PCIE_BRANCH(GccPcie0CfgAhbClk, "gcc_pcie_0_cfg_ahb_clk",
            0x7b030, 0x62008, BIT(2), CLOCK_NODE_BRANCH_VOTED, 0x7b030);
PCIE_BRANCH(GccPcie0MstrAxiClk, "gcc_pcie_0_mstr_axi_clk",
            0x7b028, 0x62008, BIT(1), 0, 0);
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
PCIE_BRANCH(GccPcie0PipeClk, "gcc_pcie_0_pipe_clk",
            0x7b03c, 0x62008, BIT(4), 0, 0);

PCIE_BRANCH(GccPcie1CfgAhbClk, "gcc_pcie_1_cfg_ahb_clk",
            0x9d02c, 0x62000, BIT(28), CLOCK_NODE_BRANCH_VOTED, 0x9d02c);
PCIE_BRANCH(GccPcie1MstrAxiClk, "gcc_pcie_1_mstr_axi_clk",
            0x9d024, 0x62000, BIT(27), 0, 0);
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
PCIE_BRANCH(GccPcie1PipeClk, "gcc_pcie_1_pipe_clk",
            0x9d040, 0x62000, BIT(30), 0, 0);

STATIC ClockNode *mGccClocks[SM8450_CLOCK_GCC_CLOCK_COUNT] = {
  [GCC_AGGRE_NOC_PCIE_0_AXI_CLK] = &GccAggreNocPcie0AxiClk,
  [GCC_AGGRE_NOC_PCIE_1_AXI_CLK] = &GccAggreNocPcie1AxiClk,
  [GCC_DDRSS_PCIE_SF_TBU_CLK]    = &GccDdrssPcieSfTbuClk,
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
};

STATIC ClockNode *mGccGdscs[] = {
  &GccPcie0Gdsc,
  &GccPcie1Gdsc,
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
  [CC_TYPE_GPU_CC]   = { .Address = 0x3d90000, .Size = 0xa000 },
  [CC_TYPE_DISP_CC]  = { .Address = 0xaf00000, .Size = 0x20000 },
  [CC_TYPE_APSS_CC]  = { .Address = 0x17a80000, .Size = 0x21000 },
  [CC_TYPE_MC_CC]    = { .Address = 0x190ba000, .Size = 0x54 },
};

STATIC ClockDriverContext Sm8450ClockContext = {
  .ClockControllerCount = SM8450_CLOCK_CONTROLLER_COUNT,
  .ClockControllers     = Sm8450ClockControllers,
};

typedef struct {
  CR_DAL_CLOCK_RESOURCE_KIND  Kind;
  CONST CHAR8                *Id;
  CONST CHAR8                *NativeId;
  UINT32                      Flags;
} SM8450_NATIVE_CLOCK_RESOURCE;

/*
 * Waipio's native ClockDxe names differ from the Linux-derived target names
 * in a few places.  In the packaged ClockDxe, the pipe clock descriptors at
 * RVAs 0x22fb0/0x23150 point to mux domains at 0x29938/0x29ad8.  Those domains
 * have no BSP or active mux configuration; their DomainMux ConfigMux routine
 * at RVA 0xf374 writes the requested selector to 0x17b060/0x19d064.  Request
 * external source zero explicitly before each first native vote so cold and
 * fully released retry paths select the PHY input, matching Linux
 * phy_mux_enable().  PCIe1 PHY AUX is another no-BSP mux whose source and leaf
 * aliases resolve to the native leaf.  The PCIe controller BCRs share the reset
 * controls exposed through their AUX clock objects; the PHY BCR names are
 * native as-is.
 */
STATIC CONST SM8450_NATIVE_CLOCK_RESOURCE mNativeClockResources[] = {
  { CrDalClockResourceClock, "gcc_pcie_0_pipe_clk_src",
    "gcc_pcie_0_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_0_pipe_clk",
    "gcc_pcie_0_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_pipe_clk_src",
    "gcc_pcie_1_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_pipe_clk",
    "gcc_pcie_1_pipe_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_phy_aux_clk_src",
    "gcc_pcie_1_phy_aux_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourceClock, "gcc_pcie_1_phy_aux_clk",
    "gcc_pcie_1_phy_aux_clk", CR_DAL_CLOCK_RESOURCE_NO_RATE |
    CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE },
  { CrDalClockResourcePowerDomain, "pcie_0_gdsc",
    "gcc_pcie_0_gdsc", 0 },
  { CrDalClockResourcePowerDomain, "pcie_1_gdsc",
    "gcc_pcie_1_gdsc", 0 },
  { CrDalClockResourceReset, "gcc_pcie_0_bcr",
    "gcc_pcie_0_aux_clk", 0 },
  { CrDalClockResourceReset, "gcc_pcie_1_bcr",
    "gcc_pcie_1_aux_clk", 0 },
};

CR_STATUS
CrTargetClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  UINTN Index;

  if ((Controller == NULL) || (Id == NULL) ||
      (Kind >= CrDalClockResourceMax) || (NativeId == NULL) ||
      (Flags == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  *NativeId = NULL;
  *Flags    = 0;
  if (cr_strcmp (Controller, "gcc") != 0) {
    return CR_UNSUPPORTED;
  }

  for (Index = 0; Index < ARRAY_SIZE (mNativeClockResources); Index++) {
    if ((mNativeClockResources[Index].Kind == Kind) &&
        (cr_strcmp (mNativeClockResources[Index].Id, Id) == 0)) {
      *NativeId = mNativeClockResources[Index].NativeId;
      *Flags    = mNativeClockResources[Index].Flags;
      return CR_SUCCESS;
    }
  }

  /* All other GCC resources already use their native ClockDxe names. */
  *NativeId = Id;
  return CR_SUCCESS;
}

ClockDriverContext *CrTargetGetClockContext(VOID)
{
  return &Sm8450ClockContext;
}

CR_STATUS CrTargetClockInit(ClockDriverContext *ClockContext)
{
  if (ClockContext == NULL) {
    return CR_INVALID_PARAMETER;
  }

  /* Preserve the existing UFS ICE force-on vote used by this target. */
  CrMmioUpdateBits32(
      CLOCK_CLOCK_REGISTER(
          ClockContext, CC_TYPE_GCC, GCC_UFS_PHY_ICE_CORE_CLK, HaltRegister),
      BIT(14), BIT(14));
  return CR_SUCCESS;
}

CR_STATUS
CrTargetClockReset(
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  UINTN Offset;

  if (Controller == NULL || Id == NULL ||
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
  } else {
    return CR_UNSUPPORTED;
  }

  (VOID)CrMmioWrite32(0x00100000U + Offset, Assert ? BIT(0) : 0U);
  MemoryFence();
  return CR_SUCCESS;
}
