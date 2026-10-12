/* Generated SM8350 HDK topology facts. SPDX-License-Identifier: MIT
 * Linux a53261b60834ce617cc4f3c7723dda8285c59789; sm8350.c SHA256 c7767ae96894c50ac1aebfdca697abc7d92acbbbfdeba036347de1fa70747426
 * Source Copyright (c) 2019-2020 The Linux Foundation; 2021 Linaro, GPL-2.0-only.
 * No Linux algorithm is copied. Run LahainaCrPkg/Tools/generate_interconnect.py.
 * Provider IDs below are flattened Crane adapter IDs, not Linux binding IDs.
 * All selected BCM members are retained; links to unexposed blocks are omitted.
 */
#include <Library/CrTargetInterconnectLib.h>
#include <oskal/common.h>
STATIC CONST InterconnectTargetNode mNodes[] = {
  {"chm_apps", 2, 32, 0, 2},
  {"ebi", 4, 4, 2, 0},
  {"llcc_mc", 4, 4, 2, 1},
  {"qhs_lpass_cfg", 1, 4, 3, 0},
  {"qhs_pdm", 1, 4, 3, 0},
  {"qhs_qspi", 1, 4, 3, 0},
  {"qhs_sdc2", 1, 4, 3, 0},
  {"qhs_sdc4", 1, 4, 3, 0},
  {"qnm_aggre2_noc", 1, 16, 3, 1},
  {"qnm_gemnoc_cnoc", 1, 16, 4, 5},
  {"qnm_gemnoc_pcie", 1, 8, 9, 0},
  {"qnm_snoc_sf", 1, 16, 9, 2},
  {"qns_a2noc_snoc", 1, 16, 11, 1},
  {"qns_gem_noc_cnoc", 1, 16, 12, 1},
  {"qns_gemnoc_sf", 1, 16, 13, 1},
  {"qns_llcc", 4, 16, 14, 1},
  {"xm_sdc2", 1, 8, 15, 1},
};
STATIC CONST UINT16 mLinks[] = {13, 15, 1, 14, 3, 4, 5, 6, 7, 13, 15, 8, 9, 11, 2, 12};
STATIC CONST InterconnectTargetBcm mBcms[] = {
  {"ACV", BIT(3), FALSE, 0, 1, 0},
  {"CN0", 0, TRUE, 1, 2, 0},
  {"CN2", 0, FALSE, 3, 5, 0},
  {"MC0", 0, TRUE, 8, 1, 0},
  {"SH0", 0, TRUE, 9, 1, 0},
  {"SH4", 0, FALSE, 10, 1, 0},
  {"SN0", 0, TRUE, 11, 1, 0},
  {"SN8", 0, FALSE, 12, 1, 0},
};
STATIC CONST UINT16 mBcmNodes[] = {1, 9, 10, 3, 4, 5, 6, 7, 1, 15, 0, 14, 8};
STATIC CONST InterconnectTargetEndpoint mEndpoints[] = {
  {0x8000, 16}, /* xm_sdc2 */
  {0x8001, 1}, /* ebi */
  {0x8002, 0}, /* chm_apps */
  {0x8003, 6}, /* qhs_sdc2 */
};
STATIC CONST InterconnectTargetProvider mProviders[] = {
  {"qcom,sm8350-sdhci", 0, 4, 0, 8},
};
STATIC CONST UINT16 mProviderBcms[] = {0, 1, 2, 3, 4, 5, 6, 7};
STATIC InterconnectTargetContext mTarget = {
  .Nodes=mNodes, .NodeCount=ARRAY_SIZE(mNodes), .Links=mLinks, .LinkCount=ARRAY_SIZE(mLinks),
  .Bcms=mBcms, .BcmCount=ARRAY_SIZE(mBcms), .BcmNodes=mBcmNodes, .BcmNodeCount=ARRAY_SIZE(mBcmNodes),
  .Endpoints=mEndpoints, .EndpointCount=ARRAY_SIZE(mEndpoints),
  .Providers=mProviders, .ProviderCount=ARRAY_SIZE(mProviders),
  .ProviderBcms=mProviderBcms, .ProviderBcmCount=ARRAY_SIZE(mProviderBcms),
};
InterconnectTargetContext *CrTargetGetInterconnectContext(VOID) { return &mTarget; }
