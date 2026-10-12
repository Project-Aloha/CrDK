/* Generated selected SM8250 DISPCC facts; SPDX-License-Identifier: MIT
 * OnePlus Android cad09e061ef6cf689a4a1e54d27562e0f042236a
 * DISPCC SHA256 63eef01db44478e3be216025f672f73c652f9f3952f840bedf1791bc62d77061
 * GCC SHA256 ed208678508c983d0989f1f135fac898c5a9a91c14af1fa42cd7ea2f24cd14cd
 * DT SHA256 db41cec585d7e93c8aca0daf1d0274fa3571b5eb6646d287867737f7a3d42899
 * Register/rate facts only. Direct PLL qualification is shared ClockLib;
 * no cold analogue startup is granted by this inherited profile. */
STATIC ClockNode gcc_disp_ahb_clk = {
  .Name="gcc_disp_ahb_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .EnableRegister=0xb00c, .HaltRegister=0xb00c, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_DELAY, .KeepEnabled=TRUE,
  .HwcgRegister=0xb00c, .HwcgMsk=BIT(1),
};
STATIC ClockNode gcc_disp_xo_clk = {
  .Name="gcc_disp_xo_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .EnableRegister=0xb044, .HaltRegister=0xb044, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT_DELAY, .KeepEnabled=TRUE,
  .HwcgRegister=0x0, .HwcgMsk=0,
};
STATIC ClockNode gcc_disp_hf_axi_clk = {
  .Name="gcc_disp_hf_axi_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .EnableRegister=0xb034, .HaltRegister=0xb034, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT, .KeepEnabled=FALSE,
  .HwcgRegister=0x0, .HwcgMsk=0,
};
STATIC ClockNode gcc_disp_sf_axi_clk = {
  .Name="gcc_disp_sf_axi_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .EnableRegister=0xb038, .HaltRegister=0xb038, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT, .KeepEnabled=FALSE,
  .HwcgRegister=0x0, .HwcgMsk=0,
};
STATIC ClockNode disp_cc_pll1 = {
  .Name="disp_cc_pll1", .Type=CLOCK_NODE_TYPE_READ_ONLY_SOURCE, .ParentController=&mDisp,
  .FixedRateHz=600000000, .ReadOnlySourceCheck=CheckDisplayPll,
};
STATIC ClockRcgFreqTable disp_cc_mdss_ahb_clk_src_rates[] = {
  {.Source={0,0}, .PreDiv=1, .FrequencyHz=19200000},
};
STATIC ClockNode disp_cc_mdss_ahb_clk_src = {
  .Name="disp_cc_mdss_ahb_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x22bc, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=TRUE,
  .FreqTable=disp_cc_mdss_ahb_clk_src_rates, .FreqCount=ARRAY_SIZE(disp_cc_mdss_ahb_clk_src_rates),
};
STATIC ClockNode disp_cc_mdss_byte0_clk_src = {
  .Name="disp_cc_mdss_byte0_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x2110, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=FALSE,
};
STATIC ClockNode disp_cc_mdss_byte1_clk_src = {
  .Name="disp_cc_mdss_byte1_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x212c, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=FALSE,
};
STATIC ClockRcgFreqTable disp_cc_mdss_esc0_clk_src_rates[] = {
  {.Source={0,0}, .PreDiv=1, .FrequencyHz=19200000},
};
STATIC ClockNode disp_cc_mdss_esc0_clk_src = {
  .Name="disp_cc_mdss_esc0_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x2148, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=FALSE,
  .FreqTable=disp_cc_mdss_esc0_clk_src_rates, .FreqCount=ARRAY_SIZE(disp_cc_mdss_esc0_clk_src_rates),
};
STATIC ClockRcgFreqTable disp_cc_mdss_esc1_clk_src_rates[] = {
  {.Source={0,0}, .PreDiv=1, .FrequencyHz=19200000},
};
STATIC ClockNode disp_cc_mdss_esc1_clk_src = {
  .Name="disp_cc_mdss_esc1_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x2160, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=FALSE,
  .FreqTable=disp_cc_mdss_esc1_clk_src_rates, .FreqCount=ARRAY_SIZE(disp_cc_mdss_esc1_clk_src_rates),
};
STATIC ClockRcgFreqTable disp_cc_mdss_mdp_clk_src_rates[] = {
  {.Source={4,4}, .PreDiv=5, .FrequencyHz=200000000},
};
STATIC ClockNode disp_cc_mdss_mdp_clk_src = {
  .Name="disp_cc_mdss_mdp_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x20c8, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=TRUE,
  .FreqTable=disp_cc_mdss_mdp_clk_src_rates, .FreqCount=ARRAY_SIZE(disp_cc_mdss_mdp_clk_src_rates),
  .FixedRateHz=200000000,
};
STATIC ClockNode disp_cc_mdss_pclk0_clk_src = {
  .Name="disp_cc_mdss_pclk0_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x2098, .MNDWidth=8, .HIDWidth=5,
  .HwClockCtrl=FALSE,
};
STATIC ClockNode disp_cc_mdss_pclk1_clk_src = {
  .Name="disp_cc_mdss_pclk1_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x20b0, .MNDWidth=8, .HIDWidth=5,
  .HwClockCtrl=FALSE,
};
STATIC ClockRcgFreqTable disp_cc_mdss_vsync_clk_src_rates[] = {
  {.Source={0,0}, .PreDiv=1, .FrequencyHz=19200000},
};
STATIC ClockNode disp_cc_mdss_vsync_clk_src = {
  .Name="disp_cc_mdss_vsync_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2, .ParentController=&mDisp,
  .CmdRegister=0x20f8, .MNDWidth=0, .HIDWidth=5,
  .HwClockCtrl=FALSE,
  .FreqTable=disp_cc_mdss_vsync_clk_src_rates, .FreqCount=ARRAY_SIZE(disp_cc_mdss_vsync_clk_src_rates),
};
STATIC ClockNode *disp_cc_mdss_ahb_clk_parents[]={&disp_cc_mdss_ahb_clk_src};
STATIC CONST UINT64 disp_cc_mdss_ahb_clk_parent_rates[]={19200000};
STATIC ClockNode disp_cc_mdss_ahb_clk = {
  .Name="disp_cc_mdss_ahb_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2080, .HaltRegister=0x2080, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_ahb_clk_parents,
  .ParentRates=disp_cc_mdss_ahb_clk_parent_rates,
};
STATIC ClockNode *disp_cc_mdss_byte0_clk_parents[]={&disp_cc_mdss_byte0_clk_src};
STATIC ClockNode disp_cc_mdss_byte0_clk = {
  .Name="disp_cc_mdss_byte0_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2028, .HaltRegister=0x2028, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_byte0_clk_parents,
};
STATIC ClockNode *disp_cc_mdss_byte0_intf_clk_parents[]={&disp_cc_mdss_byte0_clk_src};
STATIC ClockNode disp_cc_mdss_byte0_intf_clk = {
  .Name="disp_cc_mdss_byte0_intf_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x202c, .HaltRegister=0x202c, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_byte0_intf_clk_parents,
  .DividerRegister=0x2128, .DividerWidth=2,
  .DividerReadOnly=FALSE,
};
STATIC ClockNode *disp_cc_mdss_byte1_clk_parents[]={&disp_cc_mdss_byte1_clk_src};
STATIC ClockNode disp_cc_mdss_byte1_clk = {
  .Name="disp_cc_mdss_byte1_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2030, .HaltRegister=0x2030, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_byte1_clk_parents,
};
STATIC ClockNode *disp_cc_mdss_byte1_intf_clk_parents[]={&disp_cc_mdss_byte1_clk_src};
STATIC ClockNode disp_cc_mdss_byte1_intf_clk = {
  .Name="disp_cc_mdss_byte1_intf_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2034, .HaltRegister=0x2034, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_byte1_intf_clk_parents,
  .DividerRegister=0x2144, .DividerWidth=2,
  .DividerReadOnly=FALSE,
};
STATIC ClockNode *disp_cc_mdss_esc0_clk_parents[]={&disp_cc_mdss_esc0_clk_src};
STATIC CONST UINT64 disp_cc_mdss_esc0_clk_parent_rates[]={19200000};
STATIC ClockNode disp_cc_mdss_esc0_clk = {
  .Name="disp_cc_mdss_esc0_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2038, .HaltRegister=0x2038, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_esc0_clk_parents,
  .ParentRates=disp_cc_mdss_esc0_clk_parent_rates,
};
STATIC ClockNode *disp_cc_mdss_esc1_clk_parents[]={&disp_cc_mdss_esc1_clk_src};
STATIC CONST UINT64 disp_cc_mdss_esc1_clk_parent_rates[]={19200000};
STATIC ClockNode disp_cc_mdss_esc1_clk = {
  .Name="disp_cc_mdss_esc1_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x203c, .HaltRegister=0x203c, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_esc1_clk_parents,
  .ParentRates=disp_cc_mdss_esc1_clk_parent_rates,
};
STATIC ClockNode *disp_cc_mdss_mdp_clk_parents[]={&disp_cc_pll1,&disp_cc_mdss_mdp_clk_src};
STATIC CONST UINT64 disp_cc_mdss_mdp_clk_parent_rates[]={600000000,200000000};
STATIC ClockNode disp_cc_mdss_mdp_clk = {
  .Name="disp_cc_mdss_mdp_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x200c, .HaltRegister=0x200c, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=2, .Parents=disp_cc_mdss_mdp_clk_parents,
  .ParentRates=disp_cc_mdss_mdp_clk_parent_rates,
  .FixedRateHz=200000000,
};
STATIC ClockNode *disp_cc_mdss_mdp_lut_clk_parents[]={&disp_cc_pll1,&disp_cc_mdss_mdp_clk_src};
STATIC CONST UINT64 disp_cc_mdss_mdp_lut_clk_parent_rates[]={600000000,200000000};
STATIC ClockNode disp_cc_mdss_mdp_lut_clk = {
  .Name="disp_cc_mdss_mdp_lut_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x201c, .HaltRegister=0x201c, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT | CLOCK_NODE_BRANCH_VOTED,
  .ParentCount=2, .Parents=disp_cc_mdss_mdp_lut_clk_parents,
  .ParentRates=disp_cc_mdss_mdp_lut_clk_parent_rates,
  .FixedRateHz=200000000,
};
STATIC ClockNode *disp_cc_mdss_pclk0_clk_parents[]={&disp_cc_mdss_pclk0_clk_src};
STATIC ClockNode disp_cc_mdss_pclk0_clk = {
  .Name="disp_cc_mdss_pclk0_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2004, .HaltRegister=0x2004, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_pclk0_clk_parents,
};
STATIC ClockNode *disp_cc_mdss_pclk1_clk_parents[]={&disp_cc_mdss_pclk1_clk_src};
STATIC ClockNode disp_cc_mdss_pclk1_clk = {
  .Name="disp_cc_mdss_pclk1_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2008, .HaltRegister=0x2008, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_pclk1_clk_parents,
};
STATIC ClockNode *disp_cc_mdss_vsync_clk_parents[]={&disp_cc_mdss_vsync_clk_src};
STATIC CONST UINT64 disp_cc_mdss_vsync_clk_parent_rates[]={19200000};
STATIC ClockNode disp_cc_mdss_vsync_clk = {
  .Name="disp_cc_mdss_vsync_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mDisp,
  .EnableRegister=0x2024, .HaltRegister=0x2024, .EnableMsk=BIT(0),
  .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .ParentCount=1, .Parents=disp_cc_mdss_vsync_clk_parents,
  .ParentRates=disp_cc_mdss_vsync_clk_parent_rates,
};
STATIC ClockNode disp_cc_mdss_core_gdsc = {
  .Name="disp_cc_mdss_core_gdsc", .Type=CLOCK_NODE_TYPE_GDSC, .ParentController=&mDisp,
  .GdscRegister=0x3000, .PwrStsFlag=CLOCK_NODE_GDSC_PWR_STS_OFF_ON,
  .GdscFlags=CLOCK_NODE_GDSC_FLAG_HW_CTL | CLOCK_NODE_GDSC_FLAG_RETAIN_FF_EN,
};
STATIC ClockNode *mDisplayClocks[]={&disp_cc_pll1,
  &disp_cc_mdss_ahb_clk_src,
  &disp_cc_mdss_byte0_clk_src,
  &disp_cc_mdss_byte1_clk_src,
  &disp_cc_mdss_esc0_clk_src,
  &disp_cc_mdss_esc1_clk_src,
  &disp_cc_mdss_mdp_clk_src,
  &disp_cc_mdss_pclk0_clk_src,
  &disp_cc_mdss_pclk1_clk_src,
  &disp_cc_mdss_vsync_clk_src,
  &disp_cc_mdss_ahb_clk,
  &disp_cc_mdss_byte0_clk,
  &disp_cc_mdss_byte0_intf_clk,
  &disp_cc_mdss_byte1_clk,
  &disp_cc_mdss_byte1_intf_clk,
  &disp_cc_mdss_esc0_clk,
  &disp_cc_mdss_esc1_clk,
  &disp_cc_mdss_mdp_clk,
  &disp_cc_mdss_mdp_lut_clk,
  &disp_cc_mdss_pclk0_clk,
  &disp_cc_mdss_pclk1_clk,
  &disp_cc_mdss_vsync_clk,
};
STATIC ClockNode *mDisplayGdscs[]={&disp_cc_mdss_core_gdsc};
