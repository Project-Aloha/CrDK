/* Generated SM8250 HDK topology facts. SPDX-License-Identifier: MIT
 * Linux a53261b60834ce617cc4f3c7723dda8285c59789; sm8250.c SHA256 a8e8219be89e6e88c5cccf4207431a5d727678f614cc750e78fed4da08ce0c22
 * Source Copyright (c) 2020 The Linux Foundation, GPL-2.0-only.
 * No Linux algorithm is copied. Run KonaCrPkg/Tools/generate_interconnect.py.
 * Provider IDs below are flattened Crane adapter IDs, not Linux binding IDs.
 * All selected BCM members are retained; links to unexposed blocks are omitted.
 */
#include <Library/CrTargetInterconnectLib.h>
#include <oskal/common.h>
STATIC CONST InterconnectTargetNode mNodes[] = {
  {"chm_apps", 2, 32, 0, 2},
  {"ebi", 4, 4, 2, 0},
  {"llcc_mc", 4, 4, 2, 1},
  {"qhm_qup0", 1, 4, 3, 1},
  {"qhm_qup1", 1, 4, 4, 1},
  {"qhs_a1_noc_cfg", 1, 4, 5, 0},
  {"qhs_a2_noc_cfg", 1, 4, 5, 0},
  {"qhs_ahb2phy0", 1, 4, 5, 0},
  {"qhs_ahb2phy1", 1, 4, 5, 0},
  {"qhs_aoss", 1, 4, 5, 0},
  {"qhs_camera_cfg", 1, 4, 5, 0},
  {"qhs_clk_ctl", 1, 4, 5, 0},
  {"qhs_compute_dsp", 1, 4, 5, 0},
  {"qhs_cpr_cx", 1, 4, 5, 0},
  {"qhs_cpr_mmcx", 1, 4, 5, 0},
  {"qhs_cpr_mx", 1, 4, 5, 0},
  {"qhs_crypto0_cfg", 1, 4, 5, 0},
  {"qhs_cx_rdpm", 1, 4, 5, 0},
  {"qhs_dcc_cfg", 1, 4, 5, 0},
  {"qhs_ddrss_cfg", 1, 4, 5, 0},
  {"qhs_display_cfg", 1, 4, 5, 0},
  {"qhs_gpuss_cfg", 1, 8, 5, 0},
  {"qhs_imem_cfg", 1, 4, 5, 0},
  {"qhs_ipa", 1, 4, 5, 0},
  {"qhs_ipc_router", 1, 4, 5, 0},
  {"qhs_lpass_cfg", 1, 4, 5, 0},
  {"qhs_mnoc_cfg", 1, 4, 5, 0},
  {"qhs_npu_cfg", 1, 4, 5, 0},
  {"qhs_pcie0_cfg", 1, 4, 5, 0},
  {"qhs_pcie1_cfg", 1, 4, 5, 0},
  {"qhs_pcie_modem_cfg", 1, 4, 5, 0},
  {"qhs_pdm", 1, 4, 5, 0},
  {"qhs_pimem_cfg", 1, 4, 5, 0},
  {"qhs_prng", 1, 4, 5, 0},
  {"qhs_qdss_cfg", 1, 4, 5, 0},
  {"qhs_qspi", 1, 4, 5, 0},
  {"qhs_qup0", 1, 4, 5, 0},
  {"qhs_qup1", 1, 4, 5, 0},
  {"qhs_qup2", 1, 4, 5, 0},
  {"qhs_sdc2", 1, 4, 5, 0},
  {"qhs_sdc4", 1, 4, 5, 0},
  {"qhs_snoc_cfg", 1, 4, 5, 0},
  {"qhs_tcsr", 1, 4, 5, 0},
  {"qhs_tlmm0", 1, 4, 5, 0},
  {"qhs_tlmm1", 1, 4, 5, 0},
  {"qhs_tlmm2", 1, 4, 5, 0},
  {"qhs_tsif", 1, 4, 5, 0},
  {"qhs_ufs_card_cfg", 1, 4, 5, 0},
  {"qhs_ufs_mem_cfg", 1, 4, 5, 0},
  {"qhs_usb3_0", 1, 4, 5, 0},
  {"qhs_usb3_1", 1, 4, 5, 0},
  {"qhs_venus_cfg", 1, 4, 5, 0},
  {"qhs_vsense_ctrl_cfg", 1, 4, 5, 0},
  {"qnm_aggre1_noc", 1, 16, 5, 1},
  {"qnm_aggre2_noc", 1, 16, 6, 1},
  {"qnm_camnoc_hf", 2, 32, 7, 1},
  {"qnm_gemnoc", 1, 16, 8, 1},
  {"qnm_mnoc_hf", 2, 32, 9, 1},
  {"qnm_pcie", 1, 16, 10, 2},
  {"qnm_snoc", 1, 8, 12, 49},
  {"qnm_snoc_sf", 1, 16, 61, 2},
  {"qns_a1noc_snoc", 1, 16, 63, 1},
  {"qns_a2noc_snoc", 1, 16, 64, 1},
  {"qns_cnoc", 1, 8, 65, 1},
  {"qns_cnoc_a2noc", 1, 8, 66, 0},
  {"qns_gem_noc_snoc", 1, 16, 66, 1},
  {"qns_gemnoc_sf", 1, 16, 67, 1},
  {"qns_llcc", 4, 16, 68, 1},
  {"qns_mem_noc_hf", 2, 32, 69, 1},
  {"qns_pcie_mem_noc", 1, 16, 70, 1},
  {"qns_pcie_modem_mem_noc", 1, 16, 71, 1},
  {"qup0_core_master", 1, 4, 72, 1},
  {"qup0_core_slave", 1, 4, 73, 0},
  {"qup1_core_master", 1, 4, 73, 1},
  {"qup1_core_slave", 1, 4, 74, 0},
  {"qup2_core_master", 1, 4, 74, 0},
  {"qxm_mdp0", 1, 32, 74, 1},
  {"qxm_mdp1", 1, 32, 75, 1},
  {"srvc_cnoc", 1, 4, 76, 0},
  {"xm_pcie3_0", 1, 8, 76, 1},
  {"xm_pcie3_1", 1, 8, 77, 1},
  {"xm_pcie3_modem", 1, 8, 78, 1},
  {"xm_qdss_dap", 1, 8, 79, 50},
  {"xm_sdc2", 1, 8, 129, 1},
  {"xm_ufs_mem", 1, 8, 130, 1},
  {"xm_usb3_0", 1, 8, 131, 1},
  {"xm_usb3_1", 1, 8, 132, 1},
};
STATIC CONST UINT16 mLinks[] = {67, 65, 1, 62, 61, 66, 66, 68, 63, 67, 67, 65, 12, 10, 44, 43, 40, 45, 39, 26, 48, 41, 31, 17, 29, 6, 34, 20, 30, 42, 18, 19, 24, 28, 14, 27, 7, 8, 21, 51, 46, 23, 22, 49, 78, 47, 50, 25, 13, 5, 9, 33, 52, 35, 16, 32, 15, 36, 37, 38, 11, 67, 65, 53, 54, 59, 56, 60, 2, 57, 58, 58, 72, 74, 68, 68, 69, 69, 70, 12, 10, 44, 43, 40, 45, 39, 26, 48, 41, 31, 17, 29, 6, 34, 20, 30, 42, 18, 19, 24, 64, 28, 14, 27, 7, 8, 21, 51, 46, 23, 22, 49, 78, 47, 50, 25, 13, 5, 9, 33, 52, 35, 16, 32, 15, 36, 37, 38, 11, 62, 61, 61, 61};
STATIC CONST InterconnectTargetBcm mBcms[] = {
  {"ACV", BIT(3), FALSE, 0, 1, 0},
  {"CN0", 0, TRUE, 1, 52, 0},
  {"MC0", 0, TRUE, 53, 1, 0},
  {"MM0", 0, TRUE, 54, 1, 0},
  {"MM1", 0, FALSE, 55, 3, 0},
  {"QUP0", 0, FALSE, 58, 3, 0},
  {"SH0", 0, TRUE, 61, 1, 0},
  {"SH4", 0, FALSE, 62, 1, 0},
  {"SN0", 0, TRUE, 63, 1, 0},
  {"SN11", 0, FALSE, 64, 1, 0},
  {"SN12", 0, FALSE, 65, 2, 0},
  {"SN7", 0, FALSE, 67, 1, 0},
  {"SN8", 0, FALSE, 68, 1, 0},
};
STATIC CONST UINT16 mBcmNodes[] = {1, 59, 82, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 64, 78, 1, 68, 55, 76, 77, 71, 73, 75, 67, 0, 66, 56, 70, 69, 53, 54};
STATIC CONST InterconnectTargetEndpoint mEndpoints[] = {
  {0x3000, 84}, /* xm_ufs_mem */
  {0x3001, 1}, /* ebi */
  {0x3002, 0}, /* chm_apps */
  {0x3003, 48}, /* qhs_ufs_mem_cfg */
  {0x4000, 76}, /* qxm_mdp0 */
  {0x4001, 1}, /* ebi */
  {0x4002, 0}, /* chm_apps */
  {0x4003, 20}, /* qhs_display_cfg */
  {0x4004, 77}, /* qxm_mdp1 */
  {0x4005, 1}, /* ebi */
  {0x5100, 71}, /* qup0_core_master */
  {0x5101, 72}, /* qup0_core_slave */
  {0x5102, 0}, /* chm_apps */
  {0x5103, 36}, /* qhs_qup0 */
  {0x5104, 3}, /* qhm_qup0 */
  {0x5105, 1}, /* ebi */
  {0x5300, 73}, /* qup1_core_master */
  {0x5301, 74}, /* qup1_core_slave */
  {0x5302, 0}, /* chm_apps */
  {0x5303, 37}, /* qhs_qup1 */
  {0x5304, 4}, /* qhm_qup1 */
  {0x5305, 1}, /* ebi */
  {0x6000, 85}, /* xm_usb3_0 */
  {0x6001, 1}, /* ebi */
  {0x6002, 0}, /* chm_apps */
  {0x6003, 49}, /* qhs_usb3_0 */
  {0x6004, 86}, /* xm_usb3_1 */
  {0x6005, 1}, /* ebi */
  {0x6006, 0}, /* chm_apps */
  {0x6007, 50}, /* qhs_usb3_1 */
  {0x8000, 83}, /* xm_sdc2 */
  {0x8001, 1}, /* ebi */
  {0x8002, 0}, /* chm_apps */
  {0x8003, 39}, /* qhs_sdc2 */
  {0x5200, 73}, /* qup1_core_master */
  {0x5201, 74}, /* qup1_core_slave */
  {0x5202, 0}, /* chm_apps */
  {0x5203, 37}, /* qhs_qup1 */
  {0x1000, 79}, /* xm_pcie3_0 */
  {0x1001, 1}, /* ebi */
  {0x1002, 0}, /* chm_apps */
  {0x1003, 28}, /* qhs_pcie0_cfg */
  {0x1004, 80}, /* xm_pcie3_1 */
  {0x1005, 1}, /* ebi */
  {0x1006, 0}, /* chm_apps */
  {0x1007, 29}, /* qhs_pcie1_cfg */
  {0x1008, 81}, /* xm_pcie3_modem */
  {0x1009, 1}, /* ebi */
  {0x100a, 0}, /* chm_apps */
  {0x100b, 30}, /* qhs_pcie_modem_cfg */
};
STATIC CONST InterconnectTargetProvider mProviders[] = {
  {"qcom,sm8250-ufs", 0, 4, 0, 13},
  {"qcom,sm8250-mdp", 4, 6, 0, 13},
  {"qcom,sm8250-qup0", 10, 6, 0, 13},
  {"qcom,sm8250-qup1", 16, 6, 0, 13},
  {"qcom,sm8250-dwc3", 22, 8, 0, 13},
  {"qcom,sm8250-sdhci", 30, 4, 0, 13},
  {"qcom,sm8250-debug-uart", 34, 4, 0, 13},
  {"qcom,sm8250-pcie", 38, 12, 0, 13},
};
STATIC CONST UINT16 mProviderBcms[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
STATIC InterconnectTargetContext mTarget = {
  .Nodes=mNodes, .NodeCount=ARRAY_SIZE(mNodes), .Links=mLinks, .LinkCount=ARRAY_SIZE(mLinks),
  .Bcms=mBcms, .BcmCount=ARRAY_SIZE(mBcms), .BcmNodes=mBcmNodes, .BcmNodeCount=ARRAY_SIZE(mBcmNodes),
  .Endpoints=mEndpoints, .EndpointCount=ARRAY_SIZE(mEndpoints),
  .Providers=mProviders, .ProviderCount=ARRAY_SIZE(mProviders),
  .ProviderBcms=mProviderBcms, .ProviderBcmCount=ARRAY_SIZE(mProviderBcms),
};
InterconnectTargetContext *CrTargetGetInterconnectContext(VOID) { return &mTarget; }
