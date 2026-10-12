/* Generated SM8150 GCC facts; SPDX-License-Identifier: MIT
 * Linux and Android source identities: HanaCrPkg/Tests/sm8150-clock-facts.json.
 * Source Copyright (c) 2017-2019 The Linux Foundation, GPL-2.0.
 * Register/rate data only; no upstream driver algorithm is copied.
 * Regenerate with HanaCrPkg/Tools/generate_clocks.py. */
#define SM8150_CLOCK_COUNT 201U

STATIC ClockRcgFreqTable gcc_sdcc2_apps_clk_src_rates[] = {
  {.Source={0, 0}, .PreDiv=23, .M=1, .N=4, .FrequencyHz=400000},
  {.Source={1, 1}, .PreDiv=23, .M=1, .N=2, .FrequencyHz=25000000},
  {.Source={1, 1}, .PreDiv=23, .M=0, .N=0, .FrequencyHz=50000000},
};
STATIC ClockNode gcc_sdcc2_apps_clk_src = {
  .Name="gcc_sdcc2_apps_clk_src", .Type=CLOCK_NODE_TYPE_ROOT_CLOCK_GENERATOR_2,
  .CmdRegister=0x1400c, .MNDWidth=8, .HIDWidth=5,
  .ParentController=&mGcc,
  .FreqTable=gcc_sdcc2_apps_clk_src_rates, .FreqCount=ARRAY_SIZE(gcc_sdcc2_apps_clk_src_rates),
};
STATIC ClockNode gcc_sdcc2_ahb_clk = {
  .Name="gcc_sdcc2_ahb_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .HaltRegister=0x14008, .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .EnableRegister=0x14008, .EnableMsk=BIT(0),
};
STATIC ClockNode *gcc_sdcc2_apps_clk_parents[] = {&gpll0, &gcc_sdcc2_apps_clk_src};
STATIC CONST UINT64 gcc_sdcc2_apps_clk_parent_minimum_rates[] = {400001, 0};
STATIC ClockNode gcc_sdcc2_apps_clk = {
  .Name="gcc_sdcc2_apps_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .HaltRegister=0x14004, .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT,
  .EnableRegister=0x14004, .EnableMsk=BIT(0),
  .Parents=gcc_sdcc2_apps_clk_parents, .ParentCount=ARRAY_SIZE(gcc_sdcc2_apps_clk_parents),
  .ParentMinimumRates=gcc_sdcc2_apps_clk_parent_minimum_rates,
};
STATIC ClockNode gcc_prng_ahb_clk = {
  .Name="gcc_prng_ahb_clk", .Type=CLOCK_NODE_TYPE_BRANCH_2, .ParentController=&mGcc,
  .HaltRegister=0x34004, .HaltCheckFlag=CLOCK_NODE_BRANCH_HALT | CLOCK_NODE_BRANCH_VOTED,
  .EnableRegister=0x52004, .EnableMsk=BIT(13),
};
STATIC ClockNode *mClocks[SM8150_CLOCK_COUNT] = {
  [197]=&gpll0,
  [129]=&gcc_sdcc2_apps_clk_src,
  [127]=&gcc_sdcc2_ahb_clk,
  [128]=&gcc_sdcc2_apps_clk,
  [72]=&gcc_prng_ahb_clk,
};
