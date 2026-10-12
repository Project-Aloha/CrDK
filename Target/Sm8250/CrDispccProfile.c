/* Unused HDK8245 selected display clock facts; no hardware operations.
 * SPDX-License-Identifier: GPL-2.0-only
 * Derived register/parent facts:
 * OnePlusOSS/android_kernel_oneplus_sm8250 commit
 * cad09e061ef6cf689a4a1e54d27562e0f042236a,
 * drivers/clk/qcom/{dispcc-kona,gcc-kona,clk-rpmh}.c
 * Copyright (c) 2018-2019, The Linux Foundation. All rights reserved.
 * Linux snapshot a53261b60834ce617cc4f3c7723dda8285c59789,
 * drivers/clk/qcom/{dispcc-sm8250,gcc-sm8250}.c (GDSC and policy cross-check).
 * Board windows and ARC supply graph: supplied hdk8245_fdt, hash below.
 * This file intentionally has no INF/DEC/target registration.
 */
#include <Library/Sm8250DispccProfile.h>
#include <Library/LucidPllCrLib.h>

STATIC CONST CR_SM8250_DISPCC_PROFILE Profile = {
  .Id = CR_SM8250_DISPCC_HDK8245_DSI0,
  .Soc = 8250, .Board = 8245, .DsiControllerMask = 1,
  .AndroidCommit = "cad09e061ef6cf689a4a1e54d27562e0f042236a",
  .MainlineCommit = "a53261b60834ce617cc4f3c7723dda8285c59789",
  .FdtSha256 = "2743c213f1c286030eb65647c693cf0e2dfa69ee2a84ceac0e17a86d82de3070",
  .Controllers = {
    { "gcc", "qcom,gcc-kona", 0x00100000, 0x001f0000 },
    { "dispcc", "qcom,kona-dispcc", 0x0af00000, 0x00020000 }
  },
  .Rcgs = {
    { "disp_cc_mdss_ahb_clk_src", CrSm8250DispccAhbSource,
      CrSm8250DispccXo, "bi_tcxo", 0x22bc, 5, 0, 0, 1, 0, 0,
      19200000, TRUE, TRUE },
    { "disp_cc_mdss_mdp_clk_src", CrSm8250DispccMdpSource,
      CrSm8250DispccPll1, "disp_cc_pll1", 0x20c8, 5, 0, 4, 5, 0, 0,
      200000000, TRUE, TRUE },
    { "disp_cc_mdss_vsync_clk_src", CrSm8250DispccVsyncSource,
      CrSm8250DispccXo, "bi_tcxo", 0x20f8, 5, 0, 0, 1, 0, 0,
      19200000, FALSE, FALSE },
    { "disp_cc_mdss_esc0_clk_src", CrSm8250DispccEsc0Source,
      CrSm8250DispccXo, "bi_tcxo", 0x2148, 5, 0, 0, 1, 0, 0,
      19200000, FALSE, FALSE },
    { "disp_cc_mdss_byte0_clk_src", CrSm8250DispccByte0Source,
      CrSm8250DispccPhy0Byte, "dsi0_phy_pll_out_byteclk", 0x2110, 5, 0, 1,
      1, 0, 0, 0, FALSE, FALSE },
    { "disp_cc_mdss_pclk0_clk_src", CrSm8250DispccPixel0Source,
      CrSm8250DispccPhy0Pixel, "dsi0_phy_pll_out_dsiclk", 0x2098, 5, 8, 1,
      1, 1, 1, 0, FALSE, FALSE }
  },
  .Branches = {
    /* Android calls AHB/XO critical. Mainline enables AHB at probe without
     * a registered branch. HF/SF differ in halt policy between originals.
     * These facts supersede the initial integration plan's GCC halt table. */
    { "gcc_disp_ahb_clk", CrSm8250DispccGcc, CrSm8250DispccNoParent,
      0xb00c, 1, 0xb00c, 0xb00c, 2,
      CrSm8250DispccHaltDelay, CrSm8250DispccProbeEnable, TRUE },
    { "gcc_disp_xo_clk", CrSm8250DispccGcc, CrSm8250DispccNoParent,
      0xb044, 1, 0xb044, 0, 0,
      CrSm8250DispccHaltDelay, CrSm8250DispccHaltPoll, TRUE },
    { "gcc_disp_hf_axi_clk", CrSm8250DispccGcc, CrSm8250DispccNoParent,
      0xb034, 1, 0xb034, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltVoted, FALSE },
    { "gcc_disp_sf_axi_clk", CrSm8250DispccGcc, CrSm8250DispccNoParent,
      0xb038, 1, 0xb038, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltVoted, FALSE },
    { "disp_cc_mdss_ahb_clk", CrSm8250DispccDisp, CrSm8250DispccAhbSource,
      0x2080, 1, 0x2080, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE },
    { "disp_cc_mdss_mdp_clk", CrSm8250DispccDisp, CrSm8250DispccMdpSource,
      0x200c, 1, 0x200c, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE },
    { "disp_cc_mdss_mdp_lut_clk", CrSm8250DispccDisp, CrSm8250DispccMdpSource,
      0x201c, 1, 0x201c, 0, 0,
      CrSm8250DispccHaltVoted, CrSm8250DispccHaltVoted, FALSE },
    { "disp_cc_mdss_vsync_clk", CrSm8250DispccDisp, CrSm8250DispccVsyncSource,
      0x2024, 1, 0x2024, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE },
    { "disp_cc_mdss_esc0_clk", CrSm8250DispccDisp, CrSm8250DispccEsc0Source,
      0x2038, 1, 0x2038, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE },
    { "disp_cc_mdss_byte0_clk", CrSm8250DispccDisp, CrSm8250DispccByte0Source,
      0x2028, 1, 0x2028, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE },
    { "disp_cc_mdss_byte0_intf_clk", CrSm8250DispccDisp, CrSm8250DispccByte0Divider,
      0x202c, 1, 0x202c, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE },
    { "disp_cc_mdss_pclk0_clk", CrSm8250DispccDisp, CrSm8250DispccPixel0Source,
      0x2004, 1, 0x2004, 0, 0,
      CrSm8250DispccHaltPoll, CrSm8250DispccHaltPoll, FALSE }
  },
  .Byte0Divider = { "disp_cc_mdss_byte0_div_clk_src",
    CrSm8250DispccByte0Divider, CrSm8250DispccByte0Source, 0x2128, 0, 2, 2, FALSE },
  .Gdsc = { "mdss_core_gdsc", 0x3000, TRUE, TRUE, TRUE, TRUE, 2, 2, 15 },
  .Pll1 = { "disp_cc_pll1", "bi_tcxo", 0x1000,
    CR_LUCID_PLL_SM8250_DISP1, 19200000, 600000000 },
  .Xo = { "bi_tcxo", "xo_board", "xo.lvl", 38400000, 19200000,
    2, 0, 3, TRUE, TRUE, TRUE },
  .Rails = { "vdd_mm", "mmcx.lvl", "mx.lvl", "cx.lvl",
    64, 192, 64, -1, 48, TRUE, TRUE }
};

CR_STATUS
CrSm8250DispccGetProfile(UINT32 Id, CONST CR_SM8250_DISPCC_PROFILE **Out)
{
  if (!Out) return CR_INVALID_PARAMETER;
  *Out = NULL;
  if (Id != CR_SM8250_DISPCC_HDK8245_DSI0) return CR_UNSUPPORTED;
  *Out = &Profile;
  return CR_SUCCESS;
}
