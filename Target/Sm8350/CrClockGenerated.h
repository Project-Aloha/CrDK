/* Generated SM8350 facts; SPDX-License-Identifier: MIT
 * Linux commit a53261b60834ce617cc4f3c7723dda8285c59789
 * gcc-sm8350.c SHA256 95356612af065c670dfdbb6f9606aee20987290ac0b584ddb9c26943a0366348
 * Regenerate: python3 Platforms/LahainaCrPkg/Tools/generate_clocks.py
 * Source includes Copyright (c) 2020 The Linux Foundation, GPL-2.0-only.
 * Curated register/ID/rate facts; no Linux implementation is copied. */
#define SM8350_GCC_SDCC2_AHB_CLK 127U
#define SM8350_GCC_SDCC2_APPS_CLK 128U
#define SM8350_GCC_SDCC2_APPS_CLK_SRC 129U
#define SM8350_CLOCK_COUNT 200U

STATIC ClockRcgFreqTable gcc_sdcc2_apps_clk_src_rates[] = {
  {.Source={0, 0}, .PreDiv=23, .M=1, .N=4, .FrequencyHz=400000},
  {.Source={6, 6}, .PreDiv=23, .M=0, .N=0, .FrequencyHz=25000000},
  {.Source={6, 6}, .PreDiv=11, .M=0, .N=0, .FrequencyHz=50000000},
};
STATIC ClockNode gcc_sdcc2_apps_clk_src = {
  .Name="gcc_sdcc2_apps_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister=0x1400c, .MNDWidth=8, .HIDWidth=5,
  .HwClockCtrl=FALSE, .ParentController=&mGcc,
  .FreqTable=gcc_sdcc2_apps_clk_src_rates, .FreqCount=ARRAY_SIZE(gcc_sdcc2_apps_clk_src_rates),
};
STATIC ClockNode gcc_sdcc2_ahb_clk = {
  .Name="gcc_sdcc2_ahb_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister=0x14008, .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .EnableRegister=0x14008, .EnableMsk=BIT(0),
  .HwcgRegister=0x0, .HwcgMsk=0,
  .ParentController=&mGcc,
};
STATIC ClockNode *gcc_sdcc2_apps_clk_parents[] = {&gpll0, &gcc_sdcc2_apps_clk_src};
STATIC ClockNode gcc_sdcc2_apps_clk = {
  .Name="gcc_sdcc2_apps_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2,
  .HaltRegister=0x14004, .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .EnableRegister=0x14004, .EnableMsk=BIT(0),
  .HwcgRegister=0x0, .HwcgMsk=0,
  .ParentController=&mGcc,
  .ParentCount=ARRAY_SIZE(gcc_sdcc2_apps_clk_parents), .Parents=gcc_sdcc2_apps_clk_parents,
};
STATIC ClockNode *mClocks[SM8350_CLOCK_COUNT] = {
  [35]=&gpll0,
  [SM8350_GCC_SDCC2_AHB_CLK]=&gcc_sdcc2_ahb_clk,
  [SM8350_GCC_SDCC2_APPS_CLK]=&gcc_sdcc2_apps_clk,
  [SM8350_GCC_SDCC2_APPS_CLK_SRC]=&gcc_sdcc2_apps_clk_src,
};
