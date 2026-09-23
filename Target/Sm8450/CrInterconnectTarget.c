/** @file
 *  SM8450 PCIe interconnect target graph.
 *
 *  The graph is the subset of linux/drivers/interconnect/qcom/sm8450.c that
 *  backs the two PCIe host-controller DT paths:
 *
 *    pcie-anoc MASTER_PCIE_{0,1} -> mc-virt SLAVE_EBI1
 *    gem-noc MASTER_APPSS_PROC  -> config-noc SLAVE_PCIE_{0,1}
 *
 *  The EFI protocol has one provider namespace for a PCIe consumer, while
 *  Linux exposes these nodes through four providers (pcie-anoc, gem-noc,
 *  config-noc, and mc-virt).  The table below is therefore a deliberately
 *  flattened *adapter* graph: endpoint IDs are synthetic, but every node
 *  name, width, directed link, BCM membership, and CmdDB BCM name is copied
 *  from the Linux descriptors.  Keeping the provider boundary explicit here
 *  avoids making PciHostBridgeLib issue raw RPMh writes while still allowing a
 *  single acquire/set/release transaction to cover a cross-provider path.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Library/CrTargetInterconnectLib.h>

enum {
  SM8450_ICC_NODE_XM_PCIE0,
  SM8450_ICC_NODE_XM_PCIE1,
  SM8450_ICC_NODE_QNS_PCIE_MEM,
  SM8450_ICC_NODE_QNM_PCIE,
  SM8450_ICC_NODE_QNS_LLCC,
  SM8450_ICC_NODE_LLCC_MC,
  SM8450_ICC_NODE_EBI,
  SM8450_ICC_NODE_CHM_APPS,
  SM8450_ICC_NODE_QNS_GEMNOC_CNOC,
  SM8450_ICC_NODE_QNM_GEMNOC_CNOC,
  SM8450_ICC_NODE_QHS_PCIE0_CFG,
  SM8450_ICC_NODE_QHS_PCIE1_CFG,
  SM8450_ICC_NODE_QNS_PCIE,
  SM8450_ICC_NODE_QNM_GEMNOC_PCIE,
  SM8450_ICC_NODE_XS_PCIE0,
  SM8450_ICC_NODE_XS_PCIE1,
  SM8450_ICC_NODE_COUNT
};

enum {
  SM8450_ICC_BCM_SN7,
  SM8450_ICC_BCM_SH1,
  SM8450_ICC_BCM_SH0,
  SM8450_ICC_BCM_MC0,
  SM8450_ICC_BCM_CN0,
  SM8450_ICC_BCM_COUNT
};

/* Directed links follow qcom_icc_node.link_nodes in the Linux driver. */
STATIC CONST UINT16 mSm8450IccLinks[] = {
    SM8450_ICC_NODE_QNS_PCIE_MEM,     /* xm_pcie3_0 */
    SM8450_ICC_NODE_QNS_PCIE_MEM,     /* xm_pcie3_1 */
    SM8450_ICC_NODE_QNM_PCIE,         /* qns_pcie_mem_noc */
    SM8450_ICC_NODE_QNS_GEMNOC_CNOC,  /* qnm_pcie */
    SM8450_ICC_NODE_QNS_LLCC,
    SM8450_ICC_NODE_LLCC_MC,          /* qns_llcc */
    SM8450_ICC_NODE_EBI,              /* llcc_mc */
    SM8450_ICC_NODE_QNS_GEMNOC_CNOC,  /* chm_apps */
    SM8450_ICC_NODE_QNS_LLCC,
    SM8450_ICC_NODE_QNS_PCIE,
    SM8450_ICC_NODE_QNM_GEMNOC_CNOC,  /* qns_gem_noc_cnoc */
    SM8450_ICC_NODE_QHS_PCIE0_CFG,    /* qnm_gemnoc_cnoc */
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_QNM_GEMNOC_PCIE,  /* qns_pcie */
    SM8450_ICC_NODE_XS_PCIE0,         /* qnm_gemnoc_pcie */
    SM8450_ICC_NODE_XS_PCIE1,
};

STATIC CONST InterconnectTargetNode mSm8450IccNodes[] = {
    {"xm_pcie3_0", 1, 8, 0, 1},
    {"xm_pcie3_1", 1, 8, 1, 1},
    {"qns_pcie_mem_noc", 1, 16, 2, 1},
    {"qnm_pcie", 1, 16, 3, 2},
    {"qns_llcc", 4, 16, 5, 1},
    {"llcc_mc", 4, 4, 6, 1},
    {"ebi", 4, 4, 7, 0},
    {"chm_apps", 3, 32, 7, 3},
    {"qns_gem_noc_cnoc", 1, 16, 10, 1},
    {"qnm_gemnoc_cnoc", 1, 16, 11, 2},
    {"qhs_pcie0_cfg", 1, 4, 0, 0},
    {"qhs_pcie1_cfg", 1, 4, 0, 0},
    {"qns_pcie", 1, 8, 13, 1},
    {"qnm_gemnoc_pcie", 1, 8, 14, 2},
    {"xs_pcie_0", 1, 8, 0, 0},
    {"xs_pcie_1", 1, 8, 0, 0},
};

/* BCM membership is copied from the Linux SM8450 descriptor. */
STATIC CONST UINT16 mSm8450IccBcmNodes[] = {
    SM8450_ICC_NODE_QNS_PCIE_MEM,    /* SN7 */
    SM8450_ICC_NODE_QNM_PCIE,        /* SH1 */
    SM8450_ICC_NODE_QNS_GEMNOC_CNOC,
    SM8450_ICC_NODE_QNS_PCIE,
    SM8450_ICC_NODE_QNS_LLCC,        /* SH0 */
    SM8450_ICC_NODE_EBI,             /* MC0 */
    SM8450_ICC_NODE_QNM_GEMNOC_CNOC, /* CN0 */
    SM8450_ICC_NODE_QNM_GEMNOC_PCIE,
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_XS_PCIE0,
    SM8450_ICC_NODE_XS_PCIE1,
};

STATIC CONST InterconnectTargetBcm mSm8450IccBcms[] = {
    {"SN7", 0, FALSE, 0, 1},
    {"SH1", 0x1, FALSE, 1, 3},
    {"SH0", 0, TRUE, 4, 1},
    {"MC0", 0, TRUE, 5, 1},
    {"CN0", 0x1, TRUE, 6, 6},
};

/* Provider-local IDs are kept distinct because Linux's paths cross providers;
 * 0x1100/0x1101 are two EFI aliases for the same mc-virt SLAVE_EBI1 node. */
STATIC CONST InterconnectTargetEndpoint mSm8450IccEndpoints[] = {
    {0x1000, SM8450_ICC_NODE_XM_PCIE0}, /* pcie-anoc MASTER_PCIE_0 */
    {0x1001, SM8450_ICC_NODE_XM_PCIE1}, /* pcie-anoc MASTER_PCIE_1 */
    {0x1100, SM8450_ICC_NODE_EBI},      /* mc-virt SLAVE_EBI1 (pcie0) */
    {0x1101, SM8450_ICC_NODE_EBI},      /* mc-virt SLAVE_EBI1 (pcie1) */
    {0x2000, SM8450_ICC_NODE_CHM_APPS}, /* gem-noc MASTER_APPSS_PROC */
    /* Linux config-noc SLAVE_PCIE_0/1, not SLAVE_PCIE_0/1_CFG. */
    {0x2100, SM8450_ICC_NODE_XS_PCIE0}, /* config-noc SLAVE_PCIE_0 */
    {0x2101, SM8450_ICC_NODE_XS_PCIE1}, /* config-noc SLAVE_PCIE_1 */
};

STATIC CONST InterconnectTargetProvider mSm8450IccProviders[] = {
    {"qcom,sm8450-pcie", 0, 7, 0, SM8450_ICC_BCM_COUNT},
};

STATIC CONST UINT16 mSm8450IccProviderBcms[] = {
    SM8450_ICC_BCM_SN7,
    SM8450_ICC_BCM_SH1,
    SM8450_ICC_BCM_SH0,
    SM8450_ICC_BCM_MC0,
    SM8450_ICC_BCM_CN0,
};

/* Qualcomm's ICB IDs are a stable public ABI, independent of Linux's
 * provider-local DT IDs.  These values match the Waipio-era icbid.h and the
 * native PcieRpLib route contract. */
enum {
  SM8450_NATIVE_ICB_MASTER_APPSS_PROC = 0,
  SM8450_NATIVE_ICB_MASTER_PCIE_0     = 65,
  SM8450_NATIVE_ICB_MASTER_PCIE_1     = 66,
  SM8450_NATIVE_ICB_SLAVE_EBI1        = 0,
  SM8450_NATIVE_ICB_SLAVE_PCIE_0      = 84,
  SM8450_NATIVE_ICB_SLAVE_PCIE_1      = 85,
};

STATIC CONST InterconnectTargetNativeRoute mSm8450NativeRoutes[] = {
    {"qcom,sm8450-pcie", 0x1000, 0x1100,
     SM8450_NATIVE_ICB_MASTER_PCIE_0, SM8450_NATIVE_ICB_SLAVE_EBI1,
     "crane-pcie0-mem"},
    {"qcom,sm8450-pcie", 0x2000, 0x2100,
     SM8450_NATIVE_ICB_MASTER_APPSS_PROC, SM8450_NATIVE_ICB_SLAVE_PCIE_0,
     "crane-pcie0-cfg"},
    {"qcom,sm8450-pcie", 0x1001, 0x1101,
     SM8450_NATIVE_ICB_MASTER_PCIE_1, SM8450_NATIVE_ICB_SLAVE_EBI1,
     "crane-pcie1-mem"},
    {"qcom,sm8450-pcie", 0x2000, 0x2101,
     SM8450_NATIVE_ICB_MASTER_APPSS_PROC, SM8450_NATIVE_ICB_SLAVE_PCIE_1,
     "crane-pcie1-cfg"},
};

STATIC CONST InterconnectTargetContext mSm8450IccContext = {
    .Nodes          = mSm8450IccNodes,
    .NodeCount      = (UINT16)(sizeof(mSm8450IccNodes) / sizeof(mSm8450IccNodes[0])),
    .Links          = mSm8450IccLinks,
    .LinkCount      = (UINT16)(sizeof(mSm8450IccLinks) / sizeof(mSm8450IccLinks[0])),
    .Bcms           = mSm8450IccBcms,
    .BcmCount       = (UINT16)(sizeof(mSm8450IccBcms) / sizeof(mSm8450IccBcms[0])),
    .BcmNodes       = mSm8450IccBcmNodes,
    .BcmNodeCount   = (UINT16)(sizeof(mSm8450IccBcmNodes) / sizeof(mSm8450IccBcmNodes[0])),
    .Endpoints      = mSm8450IccEndpoints,
    .EndpointCount  = (UINT16)(sizeof(mSm8450IccEndpoints) / sizeof(mSm8450IccEndpoints[0])),
    .Providers      = mSm8450IccProviders,
    .ProviderCount  = (UINT16)(sizeof(mSm8450IccProviders) / sizeof(mSm8450IccProviders[0])),
    .ProviderBcms   = mSm8450IccProviderBcms,
    .ProviderBcmCount = (UINT16)(sizeof(mSm8450IccProviderBcms) / sizeof(mSm8450IccProviderBcms[0])),
    .NativeRoutes   = mSm8450NativeRoutes,
    .NativeRouteCount = (UINT16)(sizeof(mSm8450NativeRoutes) / sizeof(mSm8450NativeRoutes[0])),
};

InterconnectTargetContext *
CrTargetGetInterconnectContext(VOID)
{
  return (InterconnectTargetContext *)&mSm8450IccContext;
}
