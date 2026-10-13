/** @file SM8450 GPU boot-clock graph. SPDX-License-Identifier: MIT
 *
 * Register/ID inventory: Linux gpucc-sm8450.c, gcc-sm8450.c and
 * qcom,sm8450-gpucc.h; Android gpucc-waipio.c agrees on these boot rates.
 * Linux a6xx_gmu_resume selects GMU 200 MHz and A730 hub 150 MHz.
 * This graph implements that initial profile, not GPU DVFS. PLL1/500 MHz,
 * GMU-owned GX GDSC and resets require their separate power/firmware owner.
 * Read-only dividers retain their actual hardware value; no guessed divider
 * or shared analogue PLL retune is performed.
 * Included only by Sm8450Clock.c, after GCC's fixed GPLL0 declaration.
 */

STATIC ClockNode *mGpuGpllParent[] = { &GccGpll0Fixed };
STATIC ClockNode GccGpuGpllMain = {
  .Name="gcc_gpu_gpll0_clk_src", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .EnableRegister=0x62000, .EnableMsk=BIT(15),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_DELAY, .FixedRateHz=600000000,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
  .ParentCount=1, .Parents=mGpuGpllParent,
};
STATIC ClockNode GccGpuGpllDiv = {
  .Name="gcc_gpu_gpll0_div_clk_src", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .EnableRegister=0x62000, .EnableMsk=BIT(16),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_DELAY, .FixedRateHz=300000000,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
  .ParentCount=1, .Parents=mGpuGpllParent,
};
STATIC ClockNode GccDdrssGpuAxi = {
  .Name="gcc_ddrss_gpu_axi_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .EnableRegister=0x81154, .HaltRegister=0x81154, .EnableMsk=BIT(0),
  .HwcgRegister=0x81154, .HwcgMsk=BIT(1),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_SKIP, .KeepEnabled=TRUE,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccGpuMemnoc = {
  .Name="gcc_gpu_memnoc_gfx_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .EnableRegister=0x81010, .HaltRegister=0x81010, .EnableMsk=BIT(0),
  .HwcgRegister=0x81010, .HwcgMsk=BIT(1),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_VOTED,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
};
STATIC ClockNode GccGpuSnocDvm = {
  .Name="gcc_gpu_snoc_dvm_gfx_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .EnableRegister=0x81018, .HaltRegister=0x81018, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_DELAY,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GCC],
};

/* GPUCC's divided GPLL0 parent has mux selector6, unlike main selector5.
 * Write HID encodings directly: 1.5=>2, 2=>3, 1=>1. */
STATIC ClockRcgFreqTable mGpuGmuBootFreq[] = {
  { .Source={6,6}, .PreDiv=2, .FrequencyHz=200000000 },
};
STATIC ClockRcgFreqTable mGpuHubBootFreq[] = {
  { .Source={6,6}, .PreDiv=3, .FrequencyHz=150000000 },
};
STATIC ClockRcgFreqTable mGpuXoFreq[] = {
  { .Source={0,0}, .PreDiv=1, .FrequencyHz=19200000 },
};
#define GPU_RCG(Node, NameString, Offset, Table, Rate) \
  STATIC ClockNode Node = { \
    .Name=NameString, .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, \
    .CmdRegister=Offset, .HIDWidth=5, .HwClockCtrl=TRUE, \
    .FreqCount=ARRAY_SIZE(Table), .FreqTable=Table, .FixedRateHz=Rate, \
    .ParentController=&Sm8450ClockControllers[CC_TYPE_GPU_CC], \
  }
GPU_RCG(GpuGmuSrc,"gpu_cc_gmu_clk_src",0x9318,mGpuGmuBootFreq,200000000);
GPU_RCG(GpuFfSrc,"gpu_cc_ff_clk_src",0x9474,mGpuGmuBootFreq,200000000);
GPU_RCG(GpuHubSrc,"gpu_cc_hub_clk_src",0x93ec,mGpuHubBootFreq,150000000);
GPU_RCG(GpuXoSrc,"gpu_cc_xo_clk_src",0x9010,mGpuXoFreq,19200000);
#undef GPU_RCG
STATIC ClockNode *mGpuGmuParents[]={&GccGpuGpllDiv,&GpuGmuSrc};
STATIC ClockNode *mGpuFfParents[]={&GccGpuGpllDiv,&GpuFfSrc};
STATIC ClockNode *mGpuHubParents[]={&GccGpuGpllDiv,&GpuHubSrc};
STATIC CONST UINT64 mGpu200ParentRates[]={300000000,200000000};
STATIC CONST UINT64 mGpu150ParentRates[]={300000000,150000000};
STATIC ClockNode *mGpuXoParents[]={&GpuXoSrc};
STATIC CONST UINT64 mGpuXoParentRates[]={19200000};

#define GPU_RO_DIV(Node, NameString, Offset, InputRate, ParentsList, RatesList) \
  STATIC ClockNode Node = { \
    .Name=NameString, .Type=CLOCK_NODE_TYPE_READ_ONLY_DIVIDER, \
    .DividerRegister=Offset, .DividerWidth=4, .DividerReadOnly=TRUE, \
    .DividerInputRateHz=InputRate, .ParentCount=ARRAY_SIZE(ParentsList), \
    .Parents=ParentsList, .ParentRates=RatesList, \
    .ParentController=&Sm8450ClockControllers[CC_TYPE_GPU_CC], \
  }
GPU_RO_DIV(GpuDemetDiv,"gpu_cc_demet_div_clk_src",0x9054,19200000,mGpuXoParents,mGpuXoParentRates);
GPU_RO_DIV(GpuHubAhbDiv,"gpu_cc_hub_ahb_div_clk_src",0x9430,150000000,mGpuHubParents,mGpu150ParentRates);
GPU_RO_DIV(GpuHubCxDiv,"gpu_cc_hub_cx_int_div_clk_src",0x942c,150000000,mGpuHubParents,mGpu150ParentRates);
GPU_RO_DIV(GpuXoDiv,"gpu_cc_xo_div_clk_src",0x9050,19200000,mGpuXoParents,mGpuXoParentRates);
#undef GPU_RO_DIV
STATIC ClockNode *mGpuAhbParents[]={&GpuHubAhbDiv};
STATIC ClockNode *mGpuHubCxParents[]={&GpuHubCxDiv};
STATIC ClockNode *mGpuDemetParents[]={&GpuDemetDiv};
STATIC ClockNode *mGpuMeasureParents[]={&GpuXoDiv};

#define GPU_BRANCH(Node, NameString, Offset, Halt, Aon, Rate, ParentsList, RatesList, Count) \
  STATIC ClockNode Node = { \
    .Name=NameString, .Type=CLOCK_NODE_TYPE_BRANCH_2, \
    .EnableRegister=Offset, .HaltRegister=Offset, .EnableMsk=BIT(0), \
    .HaltCheckFlag=Halt, .KeepEnabled=Aon, .FixedRateHz=Rate, \
    .Parents=ParentsList, .ParentRates=RatesList, .ParentCount=Count, \
    .ParentController=&Sm8450ClockControllers[CC_TYPE_GPU_CC], \
  }
GPU_BRANCH(GpuAhb,"gpu_cc_ahb_clk",0x911c,CLOCK_NODE_BRANCH_HALT_DELAY,FALSE,0,mGpuAhbParents,NULL,1);
GPU_BRANCH(GpuCrcAhb,"gpu_cc_crc_ahb_clk",0x9120,CLOCK_NODE_BRANCH_VOTED,FALSE,0,mGpuAhbParents,NULL,1);
GPU_BRANCH(GpuCxGmu,"gpu_cc_cx_gmu_clk",0x913c,CLOCK_NODE_BRANCH_HALT,TRUE,200000000,mGpuGmuParents,mGpu200ParentRates,2);
GPU_BRANCH(GpuCxFf,"gpu_cc_cx_ff_clk",0x914c,CLOCK_NODE_BRANCH_HALT,FALSE,200000000,mGpuFfParents,mGpu200ParentRates,2);
GPU_BRANCH(GpuCxo,"gpu_cc_cxo_clk",0x9144,CLOCK_NODE_BRANCH_HALT,FALSE,19200000,mGpuXoParents,NULL,1);
GPU_BRANCH(GpuCxoAon,"gpu_cc_cxo_aon_clk",0x9004,CLOCK_NODE_BRANCH_VOTED,FALSE,19200000,mGpuXoParents,NULL,1);
GPU_BRANCH(GpuDemet,"gpu_cc_demet_clk",0x900c,CLOCK_NODE_BRANCH_HALT,TRUE,0,mGpuDemetParents,NULL,1);
GPU_BRANCH(GpuHubCx,"gpu_cc_hub_cx_int_clk",0x9148,CLOCK_NODE_BRANCH_HALT,TRUE,0,mGpuHubCxParents,NULL,1);
GPU_BRANCH(GpuHubAon,"gpu_cc_hub_aon_clk",0x93e8,CLOCK_NODE_BRANCH_HALT,TRUE,150000000,mGpuHubParents,mGpu150ParentRates,2);
GPU_BRANCH(GpuMeasure,"gpu_cc_freq_measure_clk",0x9008,CLOCK_NODE_BRANCH_HALT,FALSE,0,mGpuMeasureParents,NULL,1);
GPU_BRANCH(GpuSmmuVote,"gpu_cc_hlos1_vote_gpu_smmu_clk",0x7000,CLOCK_NODE_BRANCH_VOTED,FALSE,0,NULL,NULL,0);
GPU_BRANCH(GpuCxApb,"gpu_cc_cx_apb_clk",0x912c,CLOCK_NODE_BRANCH_VOTED,FALSE,0,NULL,NULL,0);
GPU_BRANCH(GpuCxSnocDvm,"gpu_cc_cx_snoc_dvm_clk",0x9130,CLOCK_NODE_BRANCH_VOTED,FALSE,0,NULL,NULL,0);
GPU_BRANCH(GpuMemnoc,"gpu_cc_memnoc_gfx_clk",0x9150,CLOCK_NODE_BRANCH_HALT,FALSE,0,NULL,NULL,0);
GPU_BRANCH(GpuSleep,"gpu_cc_sleep_clk",0x9134,CLOCK_NODE_BRANCH_VOTED,FALSE,0,NULL,NULL,0);
#undef GPU_BRANCH

/* Public Linux binding IDs. Unimplemented GX clocks/analogue PLLs remain NULL. */
STATIC ClockNode *mGpuClocks[32] = {
  [0]=&GpuAhb, [1]=&GpuCrcAhb, [2]=&GpuCxApb, [3]=&GpuCxFf,
  [4]=&GpuCxGmu, [5]=&GpuCxSnocDvm, [6]=&GpuCxoAon, [7]=&GpuCxo,
  [8]=&GpuDemet, [9]=&GpuDemetDiv, [10]=&GpuFfSrc, [11]=&GpuMeasure,
  [12]=&GpuGmuSrc, [18]=&GpuSmmuVote, [19]=&GpuHubAhbDiv,
  [20]=&GpuHubAon, [21]=&GpuHubSrc, [22]=&GpuHubCx, [23]=&GpuHubCxDiv,
  [24]=&GpuMemnoc, [29]=&GpuSleep, [30]=&GpuXoSrc, [31]=&GpuXoDiv,
};

/* CX is votable; GDS_HW_CTRL supplies aggregate status. GX power-on is GMU
 * owned and intentionally has no ordinary ClockNode enable implementation. */
STATIC ClockNode GpuCxGdsc = {
  .Name="gpu_cx_gdsc", .Type=CLOCK_NODE_TYPE_GDSC,
  .GdscRegister=0x9108, .GdscHwCtrlRegister=0x953c,
  .GdscFlags=CLOCK_NODE_GDSC_FLAG_VOTABLE | CLOCK_NODE_GDSC_FLAG_RETAIN_FF_EN,
  .PwrStsFlag=CLOCK_NODE_GDSC_PWR_STS_OFF_ON,
  .ParentController=&Sm8450ClockControllers[CC_TYPE_GPU_CC],
};
STATIC ClockNode *mGpuGdscs[]={&GpuCxGdsc};
