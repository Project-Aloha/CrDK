/* Generated SM8350 topology facts. SPDX-License-Identifier: MIT
 * Linux a53261b60834ce617cc4f3c7723dda8285c59789; sm8350.c SHA256 c7767ae96894c50ac1aebfdca697abc7d92acbbbfdeba036347de1fa70747426
 * Source Copyright (c) 2019-2020 The Linux Foundation; 2021 Linaro, GPL-2.0-only.
 * No Linux algorithm is copied. Run LahainaCrPkg/Tools/generate_interconnect.py.
 * Provider IDs below are flattened Crane adapter IDs, not Linux binding IDs.
 * All selected BCM members are retained; links to unexposed blocks are omitted.
 * Selected QUP2 virtual-core nodes/BCM come from public Android lahaina.c.
 */
#include <Library/CrTargetInterconnectLib.h>
#include <oskal/common.h>
STATIC CONST InterconnectTargetNode mNodes[] = {
  {"chm_apps", 2, 32, 0, 2},
  {"ebi", 4, 4, 2, 0},
  {"llcc_mc", 4, 4, 2, 1},
  {"qhm_qup2", 1, 4, 3, 1},
  {"qhs_ahb2phy0", 1, 4, 4, 0},
  {"qhs_ahb2phy1", 1, 4, 4, 0},
  {"qhs_aoss", 1, 4, 4, 0},
  {"qhs_apss", 1, 8, 4, 0},
  {"qhs_camera_cfg", 1, 4, 4, 0},
  {"qhs_clk_ctl", 1, 4, 4, 0},
  {"qhs_compute_cfg", 1, 4, 4, 0},
  {"qhs_cpr_cx", 1, 4, 4, 0},
  {"qhs_cpr_mmcx", 1, 4, 4, 0},
  {"qhs_cpr_mx", 1, 4, 4, 0},
  {"qhs_crypto0_cfg", 1, 4, 4, 0},
  {"qhs_cx_rdpm", 1, 4, 4, 0},
  {"qhs_dcc_cfg", 1, 4, 4, 0},
  {"qhs_display_cfg", 1, 4, 4, 0},
  {"qhs_gpuss_cfg", 1, 8, 4, 0},
  {"qhs_hwkm", 1, 4, 4, 0},
  {"qhs_imem_cfg", 1, 4, 4, 0},
  {"qhs_ipa", 1, 4, 4, 0},
  {"qhs_ipc_router", 1, 4, 4, 0},
  {"qhs_lpass_cfg", 1, 4, 4, 0},
  {"qhs_mss_cfg", 1, 4, 4, 0},
  {"qhs_mx_rdpm", 1, 4, 4, 0},
  {"qhs_pcie0_cfg", 1, 4, 4, 0},
  {"qhs_pcie1_cfg", 1, 4, 4, 0},
  {"qhs_pdm", 1, 4, 4, 0},
  {"qhs_pimem_cfg", 1, 4, 4, 0},
  {"qhs_pka_wrapper_cfg", 1, 4, 4, 0},
  {"qhs_pmu_wrapper_cfg", 1, 4, 4, 0},
  {"qhs_qdss_cfg", 1, 4, 4, 0},
  {"qhs_qspi", 1, 4, 4, 0},
  {"qhs_qup0", 1, 4, 4, 0},
  {"qhs_qup1", 1, 4, 4, 0},
  {"qhs_qup2", 1, 4, 4, 0},
  {"qhs_sdc2", 1, 4, 4, 0},
  {"qhs_sdc4", 1, 4, 4, 0},
  {"qhs_security", 1, 4, 4, 0},
  {"qhs_spss_cfg", 1, 4, 4, 0},
  {"qhs_tcsr", 1, 4, 4, 0},
  {"qhs_tlmm", 1, 4, 4, 0},
  {"qhs_ufs_card_cfg", 1, 4, 4, 0},
  {"qhs_ufs_mem_cfg", 1, 4, 4, 0},
  {"qhs_usb3_0", 1, 4, 4, 0},
  {"qhs_usb3_1", 1, 4, 4, 0},
  {"qhs_venus_cfg", 1, 4, 4, 0},
  {"qhs_vsense_ctrl_cfg", 1, 4, 4, 0},
  {"qnm_aggre1_noc", 1, 16, 4, 1},
  {"qnm_aggre2_noc", 1, 16, 5, 1},
  {"qnm_camnoc_hf", 2, 32, 6, 1},
  {"qnm_gemnoc_cnoc", 1, 16, 7, 51},
  {"qnm_gemnoc_pcie", 1, 8, 58, 0},
  {"qnm_mnoc_hf", 2, 32, 58, 1},
  {"qnm_pcie", 1, 16, 59, 2},
  {"qnm_snoc_sf", 1, 16, 61, 2},
  {"qns_a1_noc_cfg", 1, 4, 63, 0},
  {"qns_a1noc_snoc", 1, 16, 63, 1},
  {"qns_a2_noc_cfg", 1, 4, 64, 0},
  {"qns_a2noc_snoc", 1, 16, 64, 1},
  {"qns_ddrss_cfg", 1, 4, 65, 0},
  {"qns_gem_noc_cnoc", 1, 16, 65, 1},
  {"qns_gemnoc_sf", 1, 16, 66, 1},
  {"qns_llcc", 4, 16, 67, 1},
  {"qns_mem_noc_hf", 2, 32, 68, 1},
  {"qns_mnoc_cfg", 1, 4, 69, 0},
  {"qns_pcie_mem_noc", 1, 16, 69, 1},
  {"qns_snoc_cfg", 1, 4, 70, 0},
  {"qup2_core_master", 1, 4, 70, 1},
  {"qup2_core_slave", 1, 4, 71, 0},
  {"qxm_crypto", 1, 8, 71, 1},
  {"qxm_mdp0", 1, 32, 72, 1},
  {"qxm_mdp1", 1, 32, 73, 1},
  {"srvc_cnoc", 1, 4, 74, 0},
  {"xm_pcie3_0", 1, 8, 74, 1},
  {"xm_pcie3_1", 1, 8, 75, 1},
  {"xm_qdss_dap", 1, 8, 76, 51},
  {"xm_sdc2", 1, 8, 127, 1},
  {"xm_ufs_mem", 1, 8, 128, 1},
  {"xm_usb3_0", 1, 8, 129, 1},
  {"xm_usb3_1", 1, 8, 130, 1},
};
STATIC CONST UINT16 mLinks[] = {62, 64, 1, 60, 63, 63, 65, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 57, 59, 61, 66, 68, 74, 64, 62, 64, 62, 64, 49, 50, 52, 56, 2, 54, 55, 70, 60, 65, 65, 67, 67, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 57, 59, 61, 66, 68, 74, 60, 58, 58, 58};
STATIC CONST InterconnectTargetBcm mBcms[] = {
  {"ACV", BIT(3), FALSE, 0, 1, 0},
  {"CE0", 0, FALSE, 1, 1, 0},
  {"CN0", 0, TRUE, 2, 2, 0},
  {"CN1", 0, FALSE, 4, 47, 0},
  {"CN2", 0, FALSE, 51, 5, 0},
  {"MC0", 0, TRUE, 56, 1, 0},
  {"MM0", 0, TRUE, 57, 1, 0},
  {"MM1", 0, FALSE, 58, 3, 0},
  {"QUP2", 0, FALSE, 61, 1, 1},
  {"SH0", 0, TRUE, 62, 1, 0},
  {"SH4", 0, FALSE, 63, 1, 0},
  {"SN0", 0, TRUE, 64, 1, 0},
  {"SN14", 0, FALSE, 65, 1, 0},
  {"SN5", 0, FALSE, 66, 1, 0},
  {"SN6", 0, FALSE, 67, 1, 0},
  {"SN7", 0, FALSE, 68, 1, 0},
  {"SN8", 0, FALSE, 69, 1, 0},
};
STATIC CONST UINT16 mBcmNodes[] = {1, 71, 52, 53, 77, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 24, 25, 26, 27, 29, 30, 31, 32, 34, 35, 36, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 57, 59, 61, 66, 68, 74, 23, 28, 33, 37, 38, 1, 65, 51, 72, 73, 70, 64, 0, 63, 67, 75, 76, 49, 50};
STATIC CONST InterconnectTargetEndpoint mEndpoints[] = {
  {0x8000, 78}, /* xm_sdc2 */
  {0x8001, 1}, /* ebi */
  {0x8002, 0}, /* chm_apps */
  {0x8003, 37}, /* qhs_sdc2 */
  {0x3000, 79}, /* xm_ufs_mem */
  {0x3001, 1}, /* ebi */
  {0x3002, 0}, /* chm_apps */
  {0x3003, 44}, /* qhs_ufs_mem_cfg */
  {0x9000, 0}, /* chm_apps */
  {0x9001, 19}, /* qhs_hwkm */
  {0x1000, 75}, /* xm_pcie3_0 */
  {0x1001, 1}, /* ebi */
  {0x1002, 0}, /* chm_apps */
  {0x1003, 26}, /* qhs_pcie0_cfg */
  {0x1004, 76}, /* xm_pcie3_1 */
  {0x1005, 1}, /* ebi */
  {0x1006, 0}, /* chm_apps */
  {0x1007, 27}, /* qhs_pcie1_cfg */
  {0x6000, 80}, /* xm_usb3_0 */
  {0x6001, 1}, /* ebi */
  {0x6002, 0}, /* chm_apps */
  {0x6003, 45}, /* qhs_usb3_0 */
  {0x6004, 81}, /* xm_usb3_1 */
  {0x6005, 1}, /* ebi */
  {0x6006, 0}, /* chm_apps */
  {0x6007, 46}, /* qhs_usb3_1 */
  {0x7000, 71}, /* qxm_crypto */
  {0x7001, 1}, /* ebi */
  {0x4000, 72}, /* qxm_mdp0 */
  {0x4001, 1}, /* ebi */
  {0x4002, 0}, /* chm_apps */
  {0x4003, 17}, /* qhs_display_cfg */
  {0x4004, 73}, /* qxm_mdp1 */
  {0x4005, 1}, /* ebi */
  {0x5400, 69}, /* qup2_core_master */
  {0x5401, 70}, /* qup2_core_slave */
  {0x5402, 0}, /* chm_apps */
  {0x5403, 36}, /* qhs_qup2 */
  {0x5404, 3}, /* qhm_qup2 */
  {0x5405, 1}, /* ebi */
};
STATIC CONST InterconnectTargetProvider mProviders[] = {
  {"qcom,sm8350-sdhci", 0, 4, 0, 17},
  {"qcom,sm8350-ufs", 4, 4, 0, 17},
  {"qcom,sm8350-rng", 8, 2, 0, 17},
  {"qcom,sm8350-pcie", 10, 8, 0, 17},
  {"qcom,sm8350-dwc3", 18, 8, 0, 17},
  {"qcom,sm8350-pil", 26, 2, 0, 17},
  {"qcom,sm8350-mdp", 28, 6, 0, 17},
  {"qcom,sm8350-qup2", 34, 6, 0, 17},
};
STATIC CONST UINT16 mProviderBcms[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
STATIC InterconnectTargetContext mTarget = {
  .Nodes=mNodes, .NodeCount=ARRAY_SIZE(mNodes), .Links=mLinks, .LinkCount=ARRAY_SIZE(mLinks),
  .Bcms=mBcms, .BcmCount=ARRAY_SIZE(mBcms), .BcmNodes=mBcmNodes, .BcmNodeCount=ARRAY_SIZE(mBcmNodes),
  .Endpoints=mEndpoints, .EndpointCount=ARRAY_SIZE(mEndpoints),
  .Providers=mProviders, .ProviderCount=ARRAY_SIZE(mProviders),
  .ProviderBcms=mProviderBcms, .ProviderBcmCount=ARRAY_SIZE(mProviderBcms),
};
InterconnectTargetContext *CrTargetGetInterconnectContext(VOID) { return &mTarget; }
