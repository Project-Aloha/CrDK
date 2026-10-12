/* Generated SM8150 HDK topology facts. SPDX-License-Identifier: MIT
 * Linux a53261b60834ce617cc4f3c7723dda8285c59789; sm8150.c SHA256 11d05c561e714aa377122c6fbe9ea042e506fb9aaddc6e9763bb9d95419542a5
 * Source Copyright (c) 2020 The Linux Foundation, GPL-2.0.
 * No Linux algorithm is copied. Run HanaCrPkg/Tools/generate_interconnect.py.
 * Provider IDs below are flattened Crane adapter IDs, not Linux binding IDs.
 * All selected BCM members are retained; links to unexposed blocks are omitted.
 */
#include <Library/CrTargetInterconnectLib.h>
#include <oskal/common.h>
STATIC CONST InterconnectTargetNode mNodes[] = {
  {"acm_apps", 2, 32, 0, 2},
  {"ebi", 4, 4, 2, 0},
  {"llcc_mc", 4, 4, 2, 1},
  {"qhm_spdm", 1, 4, 3, 1},
  {"qhs_a1_noc_cfg", 1, 4, 4, 0},
  {"qhs_a2_noc_cfg", 1, 4, 4, 0},
  {"qhs_ahb2phy_south", 1, 4, 4, 0},
  {"qhs_aop", 1, 4, 4, 0},
  {"qhs_aoss", 1, 4, 4, 0},
  {"qhs_camera_cfg", 1, 4, 4, 0},
  {"qhs_clk_ctl", 1, 4, 4, 0},
  {"qhs_compute_dsp", 1, 4, 4, 0},
  {"qhs_cpr_cx", 1, 4, 4, 0},
  {"qhs_cpr_mmcx", 1, 4, 4, 0},
  {"qhs_cpr_mx", 1, 4, 4, 0},
  {"qhs_crypto0_cfg", 1, 4, 4, 0},
  {"qhs_ddrss_cfg", 1, 4, 4, 0},
  {"qhs_display_cfg", 1, 4, 4, 0},
  {"qhs_emac_cfg", 1, 4, 4, 0},
  {"qhs_glm", 1, 4, 4, 0},
  {"qhs_gpuss_cfg", 1, 8, 4, 0},
  {"qhs_imem_cfg", 1, 4, 4, 0},
  {"qhs_ipa", 1, 4, 4, 0},
  {"qhs_mnoc_cfg", 1, 4, 4, 0},
  {"qhs_npu_cfg", 1, 4, 4, 0},
  {"qhs_pcie0_cfg", 1, 4, 4, 0},
  {"qhs_pcie1_cfg", 1, 4, 4, 0},
  {"qhs_phy_refgen_north", 1, 4, 4, 0},
  {"qhs_pimem_cfg", 1, 4, 4, 0},
  {"qhs_prng", 1, 4, 4, 0},
  {"qhs_qdss_cfg", 1, 4, 4, 0},
  {"qhs_qspi", 1, 4, 4, 0},
  {"qhs_qupv3_east", 1, 4, 4, 0},
  {"qhs_qupv3_north", 1, 4, 4, 0},
  {"qhs_qupv3_south", 1, 4, 4, 0},
  {"qhs_sdc2", 1, 4, 4, 0},
  {"qhs_sdc4", 1, 4, 4, 0},
  {"qhs_snoc_cfg", 1, 4, 4, 0},
  {"qhs_spdm", 1, 4, 4, 0},
  {"qhs_spss_cfg", 1, 4, 4, 0},
  {"qhs_ssc_cfg", 1, 4, 4, 0},
  {"qhs_tcsr", 1, 4, 4, 0},
  {"qhs_tlmm_east", 1, 4, 4, 0},
  {"qhs_tlmm_north", 1, 4, 4, 0},
  {"qhs_tlmm_south", 1, 4, 4, 0},
  {"qhs_tlmm_west", 1, 4, 4, 0},
  {"qhs_tsif", 1, 4, 4, 0},
  {"qhs_ufs_card_cfg", 1, 4, 4, 0},
  {"qhs_ufs_mem_cfg", 1, 4, 4, 0},
  {"qhs_usb3_0", 1, 4, 4, 0},
  {"qhs_usb3_1", 1, 4, 4, 0},
  {"qhs_venus_cfg", 1, 4, 4, 0},
  {"qhs_vsense_ctrl_cfg", 1, 4, 4, 0},
  {"qnm_aggre2_noc", 1, 16, 4, 2},
  {"qnm_gemnoc", 1, 8, 6, 1},
  {"qnm_snoc", 1, 8, 7, 50},
  {"qnm_snoc_sf", 1, 16, 57, 1},
  {"qns_a2noc_snoc", 1, 16, 58, 1},
  {"qns_cnoc", 1, 8, 59, 1},
  {"qns_cnoc_a2noc", 1, 8, 60, 0},
  {"qns_gem_noc_snoc", 1, 8, 60, 1},
  {"qns_gemnoc_sf", 1, 16, 61, 1},
  {"qns_llcc", 4, 16, 62, 1},
  {"srvc_aggre1_noc", 1, 4, 63, 0},
  {"srvc_aggre2_noc", 1, 4, 63, 0},
  {"srvc_cnoc", 1, 4, 63, 0},
  {"xm_sdc2", 1, 8, 63, 1},
};
STATIC CONST UINT16 mLinks[] = {62, 60, 1, 59, 61, 58, 58, 44, 11, 39, 9, 36, 35, 23, 18, 48, 42, 40, 37, 27, 34, 19, 26, 5, 30, 17, 41, 16, 13, 24, 25, 20, 51, 46, 22, 10, 7, 33, 6, 50, 65, 47, 32, 12, 45, 4, 8, 29, 52, 31, 49, 38, 15, 28, 43, 14, 21, 62, 53, 55, 54, 56, 2, 57};
STATIC CONST InterconnectTargetBcm mBcms[] = {
  {"ACV", BIT(3), FALSE, 0, 1, 0},
  {"CN0", 0, TRUE, 1, 53, 0},
  {"MC0", 0, TRUE, 54, 1, 0},
  {"SH0", 0, TRUE, 55, 1, 0},
  {"SH2", 0, FALSE, 56, 1, 0},
  {"SH5", 0, FALSE, 57, 1, 0},
  {"SN0", 0, TRUE, 58, 1, 0},
  {"SN11", 0, FALSE, 59, 1, 0},
  {"SN15", 0, FALSE, 60, 1, 0},
  {"SN3", 0, FALSE, 61, 3, 0},
};
STATIC CONST UINT16 mBcmNodes[] = {1, 3, 55, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 59, 65, 1, 62, 60, 0, 61, 53, 54, 63, 64, 58};
STATIC CONST InterconnectTargetEndpoint mEndpoints[] = {
  {0x8000, 66}, /* xm_sdc2 */
  {0x8001, 1}, /* ebi */
  {0x8002, 0}, /* acm_apps */
  {0x8003, 35}, /* qhs_sdc2 */
  {0x9000, 0}, /* acm_apps */
  {0x9001, 29}, /* qhs_prng */
};
STATIC CONST InterconnectTargetProvider mProviders[] = {
  {"qcom,sm8150-sdhci", 0, 4, 0, 10},
  {"qcom,sm8150-prng", 4, 2, 0, 10},
};
STATIC CONST UINT16 mProviderBcms[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
STATIC InterconnectTargetContext mTarget = {
  .Nodes=mNodes, .NodeCount=ARRAY_SIZE(mNodes), .Links=mLinks, .LinkCount=ARRAY_SIZE(mLinks),
  .Bcms=mBcms, .BcmCount=ARRAY_SIZE(mBcms), .BcmNodes=mBcmNodes, .BcmNodeCount=ARRAY_SIZE(mBcmNodes),
  .Endpoints=mEndpoints, .EndpointCount=ARRAY_SIZE(mEndpoints),
  .Providers=mProviders, .ProviderCount=ARRAY_SIZE(mProviders),
  .ProviderBcms=mProviderBcms, .ProviderBcmCount=ARRAY_SIZE(mProviderBcms),
};
InterconnectTargetContext *CrTargetGetInterconnectContext(VOID) { return &mTarget; }
