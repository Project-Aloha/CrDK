/** @file
 *  SM8450 PCIe/UFS/MDP/QUP interconnect target data.
 *
 *  The graph is the subset of linux/drivers/interconnect/qcom/sm8450.c that
 *  backs the two PCIe host-controller DT paths and the two UFS DT paths:
 *
 *    pcie-anoc MASTER_PCIE_{0,1} -> mc-virt SLAVE_EBI1
 *    gem-noc MASTER_APPSS_PROC  -> config-noc SLAVE_PCIE_{0,1}
 *    aggre1-noc MASTER_UFS_MEM  -> mc-virt SLAVE_EBI1
 *    gem-noc MASTER_APPSS_PROC  -> config-noc SLAVE_UFS_MEM_CFG
 *
 *  SDCC2 follows sm8450.dtsi sdhc_2: aggre2 MASTER_SDCC_2 -> mc-virt
 *  SLAVE_EBI1 and gem-noc MASTER_APPSS_PROC -> config-noc SLAVE_SDCC_2.
 *  xm_sdc2 is one channel/eight bytes and qhs_sdc2 one/four; the latter
 *  belongs to CN0. No bandwidth/voltage value is inferred by this inventory.
 *
 *  MDP uses the same local graph for display memory traffic and
 *  APPSS-to-DISPLAY_CFG access. No native ICB/NPA route namespace is needed.
 *
 *  QUP1 follows the public OnePlus Waipio downstream wrapper DT, whose
 *  A1NOC routes agree with both downstream and upstream ICC node tables:
 *    clk-virt MASTER_QUP_CORE_1 -> clk-virt SLAVE_QUP_CORE_1
 *    system-noc MASTER_A1NOC_SNOC -> gem-noc SLAVE_LLCC (snoc-llcc/config)
 *    aggre1-noc MASTER_QUP_1 -> mc-virt SLAVE_EBI1 (qup-ddr/memory)
 *  Source: OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450,
 *  877203a7cf9b79973d44a9db716490f95d820110,
 *  kernel_platform/qcom/proprietary/devicetree/qcom/waipio-qupv3.dtsi:394-398;
 *  OnePlusOSS/android_kernel_oneplus_sm8450,
 *  449760504b3a75728eb60479f5b34dc51cb263ce,
 *  drivers/interconnect/qcom/waipio.c (qhm_qup1 and bcm_qup1).
 *  Do not copy the aggre2/MASTER_QUP_1 pair from the upstream SM8450 SE DT:
 *  that provider's local ID 1 selects QUP0, not the QUP1 hardware master.
 *
 *  The EFI protocol carries provider-local endpoint IDs while Linux exposes
 *  these nodes through several providers (pcie-anoc, gem-noc, config-noc,
 *  aggre1-noc, and mc-virt). The table below is a deliberately
 *  flattened *adapter* graph: endpoint IDs are synthetic, but every node
 *  name, width, directed link, BCM membership, and CmdDB BCM name is copied
 *  from the Linux descriptors.  Keeping the provider boundary explicit here
 *  avoids making PciHostBridgeLib issue raw RPMh writes while still allowing a
 *  single acquire/set/release transaction to cover a cross-provider path.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Library/Sm8450Interconnect.h>

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
  SM8450_ICC_NODE_XM_UFS,
  SM8450_ICC_NODE_QNS_A1NOC_SNOC,
  SM8450_ICC_NODE_QNM_AGGRE1_NOC,
  SM8450_ICC_NODE_QNS_GEMNOC_SF,
  SM8450_ICC_NODE_QNM_SNOC_SF,
  SM8450_ICC_NODE_QHS_UFS_CFG,
  SM8450_ICC_NODE_QNM_MDP,
  SM8450_ICC_NODE_QNS_MEM_NOC_HF,
  SM8450_ICC_NODE_QNM_MNOC_HF,
  SM8450_ICC_NODE_QHS_DISPLAY_CFG,
  SM8450_ICC_NODE_QUP1_CORE_MASTER,
  SM8450_ICC_NODE_QUP1_CORE_SLAVE,
  SM8450_ICC_NODE_QHM_QUP1,
  SM8450_ICC_NODE_QUP0_CORE_MASTER,
  SM8450_ICC_NODE_QUP0_CORE_SLAVE,
  SM8450_ICC_NODE_QHM_QUP0,
  SM8450_ICC_NODE_QNS_A2NOC_SNOC,
  SM8450_ICC_NODE_QNM_AGGRE2_NOC,
  SM8450_ICC_NODE_XM_USB3_0,
  SM8450_ICC_NODE_QHS_USB3_0,
  SM8450_ICC_NODE_QXM_CRYPTO,
  SM8450_ICC_NODE_XM_SDCC2,
  SM8450_ICC_NODE_QHS_SDCC2,
  SM8450_ICC_NODE_QUP2_CORE_MASTER,
  SM8450_ICC_NODE_QUP2_CORE_SLAVE,
  SM8450_ICC_NODE_QHS_QUP2,
  SM8450_ICC_NODE_QNM_GPU,
  SM8450_ICC_NODE_COUNT
};

enum {
  SM8450_ICC_BCM_SN7,
  SM8450_ICC_BCM_SH1,
  SM8450_ICC_BCM_SH0,
  SM8450_ICC_BCM_MC0,
  SM8450_ICC_BCM_CN0,
  SM8450_ICC_BCM_SN2,
  SM8450_ICC_BCM_SN0,
  SM8450_ICC_BCM_MM0,
  SM8450_ICC_BCM_MM1,
  SM8450_ICC_BCM_QUP1,
  SM8450_ICC_BCM_ACV,
  SM8450_ICC_BCM_QUP0,
  SM8450_ICC_BCM_SN3,
  SM8450_ICC_BCM_CE0,
  SM8450_ICC_BCM_QUP2,
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
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QNM_GEMNOC_PCIE,  /* qns_pcie */
    SM8450_ICC_NODE_XS_PCIE0,         /* qnm_gemnoc_pcie */
    SM8450_ICC_NODE_XS_PCIE1,
    SM8450_ICC_NODE_QNS_A1NOC_SNOC,   /* xm_ufs_mem */
    SM8450_ICC_NODE_QNM_AGGRE1_NOC,   /* qns_a1noc_snoc */
    SM8450_ICC_NODE_QNS_GEMNOC_SF,    /* qnm_aggre1_noc */
    SM8450_ICC_NODE_QNM_SNOC_SF,      /* qns_gemnoc_sf */
    SM8450_ICC_NODE_QNS_GEMNOC_CNOC,  /* qnm_snoc_sf */
    SM8450_ICC_NODE_QNS_LLCC,
    SM8450_ICC_NODE_QNS_PCIE,
    SM8450_ICC_NODE_QNS_MEM_NOC_HF,   /* qnm_mdp */
    SM8450_ICC_NODE_QNM_MNOC_HF,      /* qns_mem_noc_hf */
    SM8450_ICC_NODE_QNS_LLCC,         /* qnm_mnoc_hf */
    SM8450_ICC_NODE_QUP1_CORE_SLAVE,  /* qup1_core_master */
    SM8450_ICC_NODE_QNS_A1NOC_SNOC,   /* qhm_qup1 */
    SM8450_ICC_NODE_QUP0_CORE_SLAVE,  /* qup0_core_master */
    SM8450_ICC_NODE_QNS_A2NOC_SNOC,   /* qhm_qup0 */
    SM8450_ICC_NODE_QNM_AGGRE2_NOC,   /* qns_a2noc_snoc */
    SM8450_ICC_NODE_QNS_GEMNOC_SF,    /* qnm_aggre2_noc */
    /* Appended so existing memory-path link indices remain stable. */
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QHS_USB3_0,
    SM8450_ICC_NODE_QNS_A1NOC_SNOC,   /* xm_usb3_0 */
    SM8450_ICC_NODE_QNS_A2NOC_SNOC,   /* qxm_crypto */
    SM8450_ICC_NODE_QNS_A2NOC_SNOC,   /* xm_sdc2 */
    /* Expanded configuration links; existing path offsets remain stable. */
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QHS_USB3_0,
    SM8450_ICC_NODE_QHS_SDCC2,
    /* Append debug-UART links without changing existing path offsets. */
    SM8450_ICC_NODE_QUP2_CORE_SLAVE,
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QHS_USB3_0,
    SM8450_ICC_NODE_QHS_SDCC2,
    SM8450_ICC_NODE_QHS_QUP2,
    /* Linux a53261b6 sm8450.c qnm_gpu; retain both directed links. */
    SM8450_ICC_NODE_QNS_GEMNOC_CNOC,
    SM8450_ICC_NODE_QNS_LLCC,
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
    {"qnm_gemnoc_cnoc", 1, 16, 49, 7},
    {"qhs_pcie0_cfg", 1, 4, 0, 0},
    {"qhs_pcie1_cfg", 1, 4, 0, 0},
    {"qns_pcie", 1, 8, 15, 1},
    {"qnm_gemnoc_pcie", 1, 8, 16, 2},
    {"xs_pcie_0", 1, 8, 0, 0},
    {"xs_pcie_1", 1, 8, 0, 0},
    /* Linux sm8450 aggre1-noc MASTER_UFS_MEM and config-noc
       SLAVE_UFS_MEM_CFG. */
    {"xm_ufs_mem", 1, 8, 18, 1},
    {"qns_a1noc_snoc", 1, 16, 19, 1},
    {"qnm_aggre1_noc", 1, 16, 20, 1},
    {"qns_gemnoc_sf", 1, 16, 21, 1},
    {"qnm_snoc_sf", 1, 16, 22, 3},
    {"qhs_ufs_mem_cfg", 1, 4, 0, 0},
    {"qnm_mdp", 2, 32, 25, 1},
    {"qns_mem_noc_hf", 2, 32, 26, 1},
    {"qnm_mnoc_hf", 2, 32, 27, 1},
    {"qhs_display_cfg", 1, 4, 0, 0},
    {"qup1_core_master", 1, 4, 28, 1},
    {"qup1_core_slave", 1, 4, 0, 0},
    {"qhm_qup1", 1, 4, 29, 1},
    {"qup0_core_master", 1, 4, 30, 1},
    {"qup0_core_slave", 1, 4, 0, 0},
    {"qhm_qup0", 1, 4, 31, 1},
    {"qns_a2noc_snoc", 1, 16, 32, 1},
    {"qnm_aggre2_noc", 1, 16, 33, 1},
    {"xm_usb3_0", 1, 8, 39, 1},
    {"qhs_usb3_0", 1, 4, 0, 0},
    {"qxm_crypto", 1, 8, 40, 1},
    {"xm_sdc2", 1, 8, 41, 1},
    {"qhs_sdc2", 1, 4, 0, 0},
    {"qup2_core_master", 1, 4, 48, 1},
    {"qup2_core_slave", 1, 4, 0, 0},
    {"qhs_qup2", 1, 4, 0, 0},
    {"qnm_gpu", 2, 32, 56, 2},
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
    SM8450_ICC_NODE_QHS_UFS_CFG,      /* CN0 */
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,   /* CN0 */
    SM8450_ICC_NODE_QNM_AGGRE1_NOC,   /* SN2 */
    SM8450_ICC_NODE_QNS_GEMNOC_SF,    /* SN0 */
    SM8450_ICC_NODE_QNS_MEM_NOC_HF,   /* MM0 */
    SM8450_ICC_NODE_QNM_MDP,          /* MM1 */
    SM8450_ICC_NODE_QUP1_CORE_SLAVE,  /* QUP1 */
    SM8450_ICC_NODE_EBI,             /* ACV */
    SM8450_ICC_NODE_QUP0_CORE_SLAVE,  /* QUP0 */
    SM8450_ICC_NODE_QNM_AGGRE2_NOC,   /* SN3 */
    /* Expanded CN0; retain other BCM offsets. */
    SM8450_ICC_NODE_QNM_GEMNOC_CNOC,
    SM8450_ICC_NODE_QNM_GEMNOC_PCIE,
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_XS_PCIE0,
    SM8450_ICC_NODE_XS_PCIE1,
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QHS_USB3_0,
    SM8450_ICC_NODE_QXM_CRYPTO,         /* CE0 */
    /* Expanded CN0 includes the exact Linux qhs_sdc2 member. */
    SM8450_ICC_NODE_QNM_GEMNOC_CNOC,
    SM8450_ICC_NODE_QNM_GEMNOC_PCIE,
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_XS_PCIE0,
    SM8450_ICC_NODE_XS_PCIE1,
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QHS_USB3_0,
    SM8450_ICC_NODE_QHS_SDCC2,
    /* Expanded CN0 preserves every previously represented member. */
    SM8450_ICC_NODE_QNM_GEMNOC_CNOC,
    SM8450_ICC_NODE_QNM_GEMNOC_PCIE,
    SM8450_ICC_NODE_QHS_PCIE0_CFG,
    SM8450_ICC_NODE_QHS_PCIE1_CFG,
    SM8450_ICC_NODE_XS_PCIE0,
    SM8450_ICC_NODE_XS_PCIE1,
    SM8450_ICC_NODE_QHS_UFS_CFG,
    SM8450_ICC_NODE_QHS_DISPLAY_CFG,
    SM8450_ICC_NODE_QHS_USB3_0,
    SM8450_ICC_NODE_QHS_SDCC2,
    SM8450_ICC_NODE_QHS_QUP2,
    SM8450_ICC_NODE_QUP2_CORE_SLAVE,
};

STATIC CONST InterconnectTargetBcm mSm8450IccBcms[] = {
    {"SN7", 0, FALSE, 0, 1, 0},
    {"SH1", 0x1, FALSE, 1, 3, 0},
    {"SH0", 0, TRUE, 4, 1, 0},
    {"MC0", 0, TRUE, 5, 1, 0},
    {"CN0", 0x1, TRUE, 42, 11, 0},
    {"SN2", 0, FALSE, 14, 1, 0},
    {"SN0", 0, TRUE, 15, 1, 0},
    {"MM0", 0, TRUE, 16, 1, 0},
    {"MM1", 0x1, FALSE, 17, 1, 0},
    /* The virtual QUP core clock vote uses Linux vote_scale=1, not 1000. */
    {"QUP1", 0, TRUE, 18, 1, 1},
    {"ACV", 0x8, FALSE, 19, 1, 0},
    {"QUP0", 0, TRUE, 20, 1, 1},
    {"SN3", 0, FALSE, 21, 1, 0},
    {"CE0", 0, FALSE, 31, 1, 0},
    {"QUP2", 0, TRUE, 53, 1, 1},
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
    {0x3000, SM8450_ICC_NODE_XM_UFS},    /* aggre1-noc MASTER_UFS_MEM */
    {0x3001, SM8450_ICC_NODE_EBI},      /* mc-virt SLAVE_EBI1 */
    {0x3002, SM8450_ICC_NODE_CHM_APPS}, /* gem-noc MASTER_APPSS_PROC */
    {0x3003, SM8450_ICC_NODE_QHS_UFS_CFG}, /* config-noc SLAVE_UFS_MEM_CFG */
    {0x4000, SM8450_ICC_NODE_QNM_MDP},
    {0x4001, SM8450_ICC_NODE_EBI},
    {0x4002, SM8450_ICC_NODE_CHM_APPS},
    {0x4003, SM8450_ICC_NODE_QHS_DISPLAY_CFG},
    {0x5000, SM8450_ICC_NODE_QUP1_CORE_MASTER},
    {0x5001, SM8450_ICC_NODE_QUP1_CORE_SLAVE},
    {0x5002, SM8450_ICC_NODE_QNM_AGGRE1_NOC}, /* system-noc MASTER_A1NOC_SNOC */
    {0x5003, SM8450_ICC_NODE_QNS_LLCC},       /* gem-noc SLAVE_LLCC */
    {0x5004, SM8450_ICC_NODE_QHM_QUP1},       /* aggre1-noc MASTER_QUP_1 */
    {0x5005, SM8450_ICC_NODE_EBI},           /* mc-virt SLAVE_EBI1 */
    {0x5100, SM8450_ICC_NODE_QUP0_CORE_MASTER},
    {0x5101, SM8450_ICC_NODE_QUP0_CORE_SLAVE},
    {0x5102, SM8450_ICC_NODE_QNM_AGGRE2_NOC}, /* system-noc MASTER_A2NOC_SNOC */
    {0x5103, SM8450_ICC_NODE_QNS_LLCC},
    {0x5104, SM8450_ICC_NODE_QHM_QUP0},       /* aggre2-noc MASTER_QUP_0 */
    {0x5105, SM8450_ICC_NODE_EBI},
    {0x6000, SM8450_ICC_NODE_XM_USB3_0},
    {0x6001, SM8450_ICC_NODE_EBI},
    {0x6002, SM8450_ICC_NODE_CHM_APPS},
    {0x6003, SM8450_ICC_NODE_QHS_USB3_0},
    {0x7000, SM8450_ICC_NODE_QXM_CRYPTO}, /* aggre2 MASTER_CRYPTO */
    {0x7001, SM8450_ICC_NODE_EBI},        /* mc-virt SLAVE_EBI1 */
    {0x8000, SM8450_ICC_NODE_XM_SDCC2},    /* aggre2-noc MASTER_SDCC_2 */
    {0x8001, SM8450_ICC_NODE_EBI},         /* mc-virt SLAVE_EBI1 */
    {0x8002, SM8450_ICC_NODE_CHM_APPS},    /* gem-noc MASTER_APPSS_PROC */
    {0x8003, SM8450_ICC_NODE_QHS_SDCC2},   /* config-noc SLAVE_SDCC_2 */
    /* Mainline sm8450.dtsi uart7 is geni-debug-uart: two paths only.
       QUP2 here names the virtual core/configuration fabric, not QUP0's
       DMA DDR path. Provider IDs are flattened adapter-local identities. */
    {0x5200, SM8450_ICC_NODE_QUP2_CORE_MASTER},
    {0x5201, SM8450_ICC_NODE_QUP2_CORE_SLAVE},
    {0x5202, SM8450_ICC_NODE_CHM_APPS},
    {0x5203, SM8450_ICC_NODE_QHS_QUP2},
    /* Android Waipio 877203a7 waipio-gpu.dtsi gpu_icc_path:
       gem-noc MASTER_GFX3D -> mc-virt SLAVE_EBI1. These IDs are
       flattened adapter identities, not Linux provider-local bindings. */
    {0x9000, SM8450_ICC_NODE_QNM_GPU},
    {0x9001, SM8450_ICC_NODE_EBI},
};

STATIC CONST InterconnectTargetProvider mSm8450IccProviders[] = {
  /* Provider-local IDs mirror the Linux SM8450 graph used by consumers. */
  {"qcom,sm8450-pcie", 0, 7, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-ufs", 7, 4, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-mdp", 11, 4, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-qup1", 15, 6, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-qup0", 21, 6, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-dwc3", 27, 4, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-pil", 31, 2, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-sdhci", 33, 4, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-debug-uart", 37, 4, 0, SM8450_ICC_BCM_COUNT},
  {"qcom,sm8450-gpu", 41, 2, 0, SM8450_ICC_BCM_COUNT},
};

STATIC CONST UINT16 mSm8450IccProviderBcms[] = {
    SM8450_ICC_BCM_SN7,
    SM8450_ICC_BCM_SH1,
    SM8450_ICC_BCM_SH0,
    SM8450_ICC_BCM_MC0,
    SM8450_ICC_BCM_CN0,
    SM8450_ICC_BCM_SN2,
    SM8450_ICC_BCM_SN0,
    SM8450_ICC_BCM_MM0,
    SM8450_ICC_BCM_MM1,
    SM8450_ICC_BCM_QUP1,
    SM8450_ICC_BCM_ACV,
    SM8450_ICC_BCM_QUP0,
    SM8450_ICC_BCM_SN3,
    SM8450_ICC_BCM_CE0,
    SM8450_ICC_BCM_QUP2,
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
    .NativeRoutes   = NULL,
    .NativeRouteCount = 0,
};

CONST InterconnectTargetContext *
Sm8450InterconnectGetTarget(VOID)
{
  return &mSm8450IccContext;
}
