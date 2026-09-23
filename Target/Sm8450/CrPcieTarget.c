/**
 * SM8450 PCIe target description.
 *
 * Values combine Linux sm8450 PCIe0/PCIe1 and QMP PHY descriptions with the
 * HDK board's supplies and link-speed limit. Resource owners
 * (RPMh, interconnect, clock, GPIO and SMMU) consume the metadata through the
 * PciHostBridgeLib adapter; this target library only supplies MMIO and delay
 * primitives and never reaches into those drivers.
 *
 * SPDX-License-Identifier: MIT
 */

#include <Library/CrTargetPcieLib.h>
#include <Library/pcie.h>
#include <oskal/cr_assert.h>
#include <oskal/common.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_string.h>
#include <oskal/cr_time.h>

#define SM8450_PCIE0_DOMAIN 0
#define SM8450_PCIE1_DOMAIN 1

#define SM8450_ICC_PROVIDER       "qcom,sm8450-pcie"
#define SM8450_ICC_PCIE0_MEM_SRC  0x1000U
#define SM8450_ICC_PCIE1_MEM_SRC  0x1001U
#define SM8450_ICC_PCIE0_MEM_DST  0x1100U
#define SM8450_ICC_PCIE1_MEM_DST  0x1101U
#define SM8450_ICC_CPU_SRC        0x2000U
#define SM8450_ICC_PCIE0_CFG_DST  0x2100U
#define SM8450_ICC_PCIE1_CFG_DST  0x2101U

#define SM8450_PCIE0_PARF   0x01c00000ULL
#define SM8450_PCIE0_DBI    0x60000000ULL
#define SM8450_PCIE0_ELBI   0x60000f20ULL
#define SM8450_PCIE0_ATU    0x60001000ULL
#define SM8450_PCIE0_CONFIG 0x60100000ULL

#define SM8450_PCIE1_PARF   0x01c08000ULL
#define SM8450_PCIE1_DBI    0x40000000ULL
#define SM8450_PCIE1_ELBI   0x40000f20ULL
#define SM8450_PCIE1_ATU    0x40001000ULL
#define SM8450_PCIE1_CONFIG 0x40100000ULL

#define SM8450_PCIE0_PHY 0x01c06000ULL
#define SM8450_PCIE1_PHY 0x01c0e000ULL

/* QMP v5 register layout used by sm8450 gen3x1. */
#define GEN3_SERDES  0x0000
#define GEN3_PCS     0x0200
#define GEN3_PCS_MISC 0x0600
#define GEN3_TX      0x0e00
#define GEN3_RX      0x1000

/* QMP v5.20 register layout used by sm8450 gen4x2. */
#define GEN4_SERDES  0x1000
#define GEN4_PCS     0x1200
#define GEN4_PCS_MISC 0x1400
#define GEN4_TX      0x0000
#define GEN4_RX      0x0200
#define GEN4_TX2     0x0800
#define GEN4_RX2     0x0a00

#define QMP_SW_RESET       0x000
#define QMP_STATUS         0x014
#define QMP_POWER_DOWN     0x040
#define QMP_START          0x044
#define QMP_SW_PWRDN       BIT(0)
#define QMP_REFCLK_DISABLE BIT(1)
#define QMP_STATUS_GEN3    BIT(6)
#define QMP_STATUS_GEN4    BIT(7)

#define PHY_ENTRY(phase, block, offset, value, lanes) \
  { (phase), (block), (offset), (value), (lanes) }

/* The complete SM8450 gen3 table from phy-qcom-qmp-pcie.c, expressed as
 * offsets so the portable PCIe core can program it without Linux headers. */
STATIC CONST PciePhyInitEntry mGen3PhyInit[] = {
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x094, 0x08, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x154, 0x34, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x16c, 0x08, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x058, 0x0f, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0a4, 0x42, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x110, 0x24, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x11c, 0x03, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x118, 0xb4, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x10c, 0x02, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1bc, 0x11, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0bc, 0x82, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0d4, 0x03, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0d0, 0x55, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0cc, 0x55, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0b0, 0x1a, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0ac, 0x0a, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0c4, 0x68, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0e0, 0x02, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0dc, 0xaa, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0d8, 0xab, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0b8, 0x34, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0b4, 0x14, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x158, 0x01, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x074, 0x06, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x07c, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x084, 0x36, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x078, 0x06, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x080, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x088, 0x36, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1b0, 0x1e, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1ac, 0xca, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1b8, 0x18, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1b4, 0xa2, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x010, 0x01, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x01c, 0x31, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x020, 0x01, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x024, 0xde, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x028, 0x07, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x030, 0x4c, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x034, 0x06, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x048, 0x90, 1),

    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x0e4, 0x20, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x084, 0x75, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x090, 0x3f, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x03c, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x040, 0x04, 1),

    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x15c, 0x7f, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x160, 0xff, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x16c, 0xd8, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x170, 0xdc, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x174, 0xdc, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x178, 0x5c, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x17c, 0x34, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x180, 0xa6, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x190, 0x34, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0dc, 0x00, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x04c, 0x08, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x050, 0x08, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1a4, 0x38, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0cc, 0xf0, 1),

    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x188, 0x77, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x198, 0x0b, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x0dc, 0x05, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x094, 0x00, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x054, 0x00, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x0a8, 0x0f, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x020, 0xc1, 1),

    /* RC-only overrides. */
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x050, 0x07, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x178, 0xbf, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x17c, 0x3f, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x190, 0x38, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x0d8, 0x07, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x044, 0xf0, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x0f4, 0x07, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x008, 0x09, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_RX, 0x014, 0x05, 1),
};

/* Gen4x2 entries are shared by both lanes; lane-mask 3 causes the portable
 * core to mirror TX/RX writes into the second lane block. */
STATIC CONST PciePhyInitEntry mGen4PhyInit[] = {
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x044, 0x14, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x058, 0x0f, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0a4, 0x46, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x0a8, 0x04, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x10c, 0x02, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x158, 0x12, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x15c, 0x00, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x168, 0x0a, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x16c, 0x04, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x19c, 0x88, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x17c, 0x06, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1a0, 0x14, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_SERDES, 0x1a8, 0x0f, 1),

    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x078, 0x05, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x07c, 0xf6, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x030, 0x1a, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_TX, 0x034, 0x0c, 3),

    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x020, 0x16, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1c0, 0x38, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x164, 0xcc, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x168, 0x12, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x16c, 0xcc, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x174, 0x4a, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x178, 0x29, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x17c, 0xc5, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x180, 0xad, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x184, 0xb6, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x188, 0xc0, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x18c, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x190, 0xfb, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x194, 0x0f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x198, 0xc7, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x19c, 0xef, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1a0, 0xbf, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1a4, 0xa0, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1a8, 0x81, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1ac, 0xde, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1b0, 0x7f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1b4, 0x20, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x02c, 0x3f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x030, 0x37, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x090, 0x05, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1f8, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x200, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x208, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x210, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x218, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x220, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1f4, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x1fc, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x204, 0x1f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x008, 0x0c, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x00c, 0x0a, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0dc, 0x0a, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x108, 0x0b, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x07c, 0x10, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0b4, 0x00, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0ec, 0x0f, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0c4, 0x00, 3),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_RX, 0x0c8, 0x1f, 3),

    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x1e0, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x1e4, 0x22, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x170, 0x2e, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS, 0x188, 0x99, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x108, 0x02, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x0a0, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x184, 0x28, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_BASE, PCIE_PHY_BLOCK_PCS_MISC, 0x15c, 0x2e, 1),

    /* RC SSC/PLL programming from sm8450_qmp_gen4x2_pcie_rc_serdes_tbl. */
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x01c, 0x31, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x020, 0x01, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x024, 0xde, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x028, 0x07, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x030, 0x97, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x034, 0x0c, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x048, 0x90, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x074, 0x06, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x078, 0x06, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x07c, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x080, 0x16, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x084, 0x36, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x088, 0x36, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x094, 0x08, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0ac, 0x0a, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0b0, 0x1a, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0b4, 0x14, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0b8, 0x34, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0bc, 0x82, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0c4, 0xd0, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0cc, 0x55, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0d0, 0x55, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0d4, 0x03, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0d8, 0x55, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0dc, 0x55, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x0e0, 0x05, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x154, 0x34, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_SERDES, 0x174, 0x20, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_PCS_MISC, 0x01c, 0xc1, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_PCS_MISC, 0x090, 0x00, 1),
    PHY_ENTRY(PCIE_PHY_PHASE_RC, PCIE_PHY_BLOCK_PCS_MISC, 0x0e0, 0x00, 1),
};

/* Keep every target slice expressed from the preceding slice.  The static
 * assertions below make a table edit fail the build instead of silently
 * moving a later controller onto the wrong resources. */
enum {
  SM8450_PCIE0_REGION_OFFSET = 0,
  SM8450_PCIE0_REGION_COUNT = 5,
  SM8450_PCIE1_REGION_OFFSET =
      SM8450_PCIE0_REGION_OFFSET + SM8450_PCIE0_REGION_COUNT,
  SM8450_PCIE1_REGION_COUNT = 5,
  SM8450_PCIE_REGION_TOTAL =
      SM8450_PCIE1_REGION_OFFSET + SM8450_PCIE1_REGION_COUNT,

  SM8450_PCIE0_RANGE_OFFSET = 0,
  SM8450_PCIE0_RANGE_COUNT = 2,
  SM8450_PCIE1_RANGE_OFFSET =
      SM8450_PCIE0_RANGE_OFFSET + SM8450_PCIE0_RANGE_COUNT,
  SM8450_PCIE1_RANGE_COUNT = 2,
  SM8450_PCIE_RANGE_TOTAL =
      SM8450_PCIE1_RANGE_OFFSET + SM8450_PCIE1_RANGE_COUNT,

  SM8450_PCIE0_INTERRUPT_OFFSET = 0,
  SM8450_PCIE_INTERRUPT_COUNT = 9,
  SM8450_PCIE0_INTX_OFFSET =
      SM8450_PCIE0_INTERRUPT_OFFSET + SM8450_PCIE_INTERRUPT_COUNT,
  SM8450_PCIE_INTX_COUNT = 4,
  SM8450_PCIE1_INTERRUPT_OFFSET =
      SM8450_PCIE0_INTX_OFFSET + SM8450_PCIE_INTX_COUNT,
  SM8450_PCIE1_INTX_OFFSET =
      SM8450_PCIE1_INTERRUPT_OFFSET + SM8450_PCIE_INTERRUPT_COUNT,
  SM8450_PCIE_INTERRUPT_TOTAL =
      SM8450_PCIE1_INTX_OFFSET + SM8450_PCIE_INTX_COUNT,

  SM8450_PCIE0_CLOCK_OFFSET = 0,
  SM8450_PCIE0_CLOCK_COUNT = 12,
  SM8450_PCIE1_CLOCK_OFFSET =
      SM8450_PCIE0_CLOCK_OFFSET + SM8450_PCIE0_CLOCK_COUNT,
  SM8450_PCIE1_CLOCK_COUNT = 11,
  SM8450_PCIE0_PHY_CLOCK_OFFSET =
      SM8450_PCIE1_CLOCK_OFFSET + SM8450_PCIE1_CLOCK_COUNT,
  SM8450_PCIE_PHY_CLOCK_COUNT = 5,
  SM8450_PCIE1_PHY_CLOCK_OFFSET =
      SM8450_PCIE0_PHY_CLOCK_OFFSET + SM8450_PCIE_PHY_CLOCK_COUNT,
  SM8450_PCIE_CLOCK_TOTAL =
      SM8450_PCIE1_PHY_CLOCK_OFFSET + SM8450_PCIE_PHY_CLOCK_COUNT,

  SM8450_PCIE0_RESET_OFFSET = 0,
  SM8450_PCIE1_RESET_OFFSET = 1,
  SM8450_PCIE0_PHY_RESET_OFFSET = 2,
  SM8450_PCIE1_PHY_RESET_OFFSET = 3,
  SM8450_PCIE_RESET_COUNT = 1,
  SM8450_PCIE_RESET_TOTAL = 4,

  SM8450_PCIE0_SUPPLY_OFFSET = 0,
  SM8450_PCIE_SUPPLY_COUNT = 2,
  SM8450_PCIE1_SUPPLY_OFFSET =
      SM8450_PCIE0_SUPPLY_OFFSET + SM8450_PCIE_SUPPLY_COUNT,
  SM8450_PCIE_SUPPLY_TOTAL =
      SM8450_PCIE1_SUPPLY_OFFSET + SM8450_PCIE_SUPPLY_COUNT,

  SM8450_PCIE0_GPIO_OFFSET = 0,
  SM8450_PCIE_GPIO_COUNT = 2,
  SM8450_PCIE1_GPIO_OFFSET =
      SM8450_PCIE0_GPIO_OFFSET + SM8450_PCIE_GPIO_COUNT,
  SM8450_PCIE_GPIO_TOTAL =
      SM8450_PCIE1_GPIO_OFFSET + SM8450_PCIE_GPIO_COUNT,

  SM8450_PCIE0_IOMMU_OFFSET = 0,
  SM8450_PCIE_IOMMU_COUNT = 2,
  SM8450_PCIE1_IOMMU_OFFSET =
      SM8450_PCIE0_IOMMU_OFFSET + SM8450_PCIE_IOMMU_COUNT,
  SM8450_PCIE_IOMMU_TOTAL =
      SM8450_PCIE1_IOMMU_OFFSET + SM8450_PCIE_IOMMU_COUNT,
};

STATIC CONST PcieTargetRegion mRegions[] = {
    { "parf", SM8450_PCIE0_PARF, 0x3000 },
    { "dbi", SM8450_PCIE0_DBI, 0xf1d },
    { "elbi", SM8450_PCIE0_ELBI, 0xa8 },
    { "atu", SM8450_PCIE0_ATU, 0x1000 },
    { "config", SM8450_PCIE0_CONFIG, 0x100000 },
    { "parf", SM8450_PCIE1_PARF, 0x3000 },
    { "dbi", SM8450_PCIE1_DBI, 0xf1d },
    { "elbi", SM8450_PCIE1_ELBI, 0xa8 },
    { "atu", SM8450_PCIE1_ATU, 0x1000 },
    { "config", SM8450_PCIE1_CONFIG, 0x100000 },
};

STATIC CONST PcieTargetRange mRanges[] = {
    { PCIE_RANGE_IO, 0x00000000ULL, 0x60200000ULL, 0x00100000ULL },
    { PCIE_RANGE_MEM32, 0x60300000ULL, 0x60300000ULL, 0x03d00000ULL },
    { PCIE_RANGE_IO, 0x00000000ULL, 0x40200000ULL, 0x00100000ULL },
    { PCIE_RANGE_MEM32, 0x40300000ULL, 0x40300000ULL, 0x1fd00000ULL },
};

#define IRQ(n) GIC_SPI(n)
STATIC CONST PcieTargetInterrupt mInterrupts[] = {
    { "msi0", IRQ(141), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi1", IRQ(142), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi2", IRQ(143), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi3", IRQ(144), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi4", IRQ(145), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi5", IRQ(146), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi6", IRQ(147), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi7", IRQ(148), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "global", IRQ(140), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_a", IRQ(149), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_b", IRQ(150), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_c", IRQ(151), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_d", IRQ(152), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi0", IRQ(307), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi1", IRQ(308), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi2", IRQ(309), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi3", IRQ(312), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi4", IRQ(313), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi5", IRQ(314), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi6", IRQ(374), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "msi7", IRQ(375), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "global", IRQ(306), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_a", IRQ(434), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_b", IRQ(435), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_c", IRQ(438), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
    { "int_d", IRQ(439), CR_INTERRUPT_TRIGGER_LEVEL_HIGH },
};

/* Clock identities intentionally follow the Linux clock-names list.  The
 * host adapter may collapse aliases (for example phy_pipe and pipe) while
 * preserving the ordering required by the cold initializer. */
STATIC CONST PcieTargetClock mClocks[] = {
    { "pipe_mux", "gcc", "gcc_pcie_0_pipe_clk_src", 250000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "pipe", "gcc", "gcc_pcie_0_pipe_clk", 250000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "phy_pipe", "pcie0_phy", "pcie_0_pipe_clk", 250000000,
      PCIE_CLOCK_PROVIDER_PHY_OUTPUT_ALIAS },
    { "ref", "rpmhcc", "rpmh_cxo_clk", 19200000,
      PCIE_CLOCK_PROVIDER_RPMH_ALWAYS_ON },
    { "aux", "gcc", "gcc_pcie_0_aux_clk", 19200000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "cfg", "gcc", "gcc_pcie_0_cfg_ahb_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "bus_master", "gcc", "gcc_pcie_0_mstr_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "bus_slave", "gcc", "gcc_pcie_0_slv_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "slave_q2a", "gcc", "gcc_pcie_0_slv_q2a_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "ddrss_sf_tbu", "gcc", "gcc_ddrss_pcie_sf_tbu_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "aggre0", "gcc", "gcc_aggre_noc_pcie_0_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "aggre1", "gcc", "gcc_aggre_noc_pcie_1_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },

    { "pipe_mux", "gcc", "gcc_pcie_1_pipe_clk_src", 250000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "pipe", "gcc", "gcc_pcie_1_pipe_clk", 250000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "phy_pipe", "pcie1_phy", "pcie_1_pipe_clk", 250000000,
      PCIE_CLOCK_PROVIDER_PHY_OUTPUT_ALIAS },
    { "ref", "rpmhcc", "rpmh_cxo_clk", 19200000,
      PCIE_CLOCK_PROVIDER_RPMH_ALWAYS_ON },
    { "aux", "gcc", "gcc_pcie_1_aux_clk", 19200000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "cfg", "gcc", "gcc_pcie_1_cfg_ahb_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "bus_master", "gcc", "gcc_pcie_1_mstr_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "bus_slave", "gcc", "gcc_pcie_1_slv_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "slave_q2a", "gcc", "gcc_pcie_1_slv_q2a_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "ddrss_sf_tbu", "gcc", "gcc_ddrss_pcie_sf_tbu_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "aggre1", "gcc", "gcc_aggre_noc_pcie_1_axi_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },

    { "aux", "gcc", "gcc_pcie_0_aux_clk", 19200000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "cfg_ahb", "gcc", "gcc_pcie_0_cfg_ahb_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "ref", "gcc", "gcc_pcie_0_clkref_en", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "rchng", "gcc", "gcc_pcie_0_phy_rchng_clk", 100000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "pipe", "gcc", "gcc_pcie_0_pipe_clk", 250000000,
      PCIE_CLOCK_PROVIDER_GCC },

    { "aux", "gcc", "gcc_pcie_1_phy_aux_clk", 20000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "cfg_ahb", "gcc", "gcc_pcie_1_cfg_ahb_clk", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "ref", "gcc", "gcc_pcie_1_clkref_en", 0,
      PCIE_CLOCK_PROVIDER_GCC },
    { "rchng", "gcc", "gcc_pcie_1_phy_rchng_clk", 100000000,
      PCIE_CLOCK_PROVIDER_GCC },
    { "pipe", "gcc", "gcc_pcie_1_pipe_clk", 250000000,
      PCIE_CLOCK_PROVIDER_GCC },
};

STATIC CONST PcieTargetReset mResets[] = {
    { "pci", "gcc", "gcc_pcie_0_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "pci", "gcc", "gcc_pcie_1_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "phy", "gcc", "gcc_pcie_0_phy_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
    { "phy", "gcc", "gcc_pcie_1_phy_bcr", 0, PCIE_CLOCK_PROVIDER_GCC },
};

/* HDK supply mapping from sm8450-hdk.dts. CMD DB uses ldo<pmic-id><n>,
 * not the Linux regulator label. Program each LDO voltage and PMIC5 HPM (7)
 * before enabling it; no earlier firmware regulator configuration is assumed. */
STATIC CONST PcieTargetSupply mSupplies[] = {
    { "vdda-phy", "rpmh", "ldob5", 880, 0, 7 },
    { "vdda-pll", "rpmh", "ldob6", 1200, 0, 7 },
    { "vdda-phy", "rpmh", "ldoh2", 880, 0, 7 },
    { "vdda-pll", "rpmh", "ldob6", 1200, 0, 7 },
};

STATIC CONST PcieTargetGpio mGpios[] = {
    { "perst", "tlmm", 94, "GPIO_ACTIVE_LOW" },
    { "wake", "tlmm", 96, "GPIO_ACTIVE_HIGH" },
    { "perst", "tlmm", 97, "GPIO_ACTIVE_LOW" },
    { "wake", "tlmm", 99, "GPIO_ACTIVE_HIGH" },
};

STATIC CONST PcieTargetIommuMap mIommuMaps[] = {
    { 0x0000, "apps_smmu", 0x1c00, 1 },
    { 0x0100, "apps_smmu", 0x1c01, 1 },
    { 0x0000, "apps_smmu", 0x1c80, 1 },
    { 0x0100, "apps_smmu", 0x1c81, 1 },
};

STATIC CONST PcieTargetPhy mPhys[] = {
    {
        .Label = "pcie0_phy",
        .Compatible = "qcom,sm8450-qmp-gen3x1-pcie-phy",
        .Base = SM8450_PCIE0_PHY,
        .Size = 0x2000,
        .Lanes = 1,
        .BlockOffsets = { GEN3_SERDES, GEN3_TX, GEN3_RX, 0, 0,
                          GEN3_PCS, GEN3_PCS_MISC },
        .SwReset = QMP_SW_RESET,
        .StartControl = QMP_START,
        .Status = QMP_STATUS,
        .PowerDownControl = QMP_POWER_DOWN,
        .PowerDownControlValue = QMP_SW_PWRDN | QMP_REFCLK_DISABLE,
        .StatusMask = QMP_STATUS_GEN3,
        .InitOffset = 0,
        .InitCount = ARRAY_SIZE(mGen3PhyInit),
        .ClockOffset = SM8450_PCIE0_PHY_CLOCK_OFFSET,
        .ClockCount = SM8450_PCIE_PHY_CLOCK_COUNT,
        .ResetOffset = SM8450_PCIE0_PHY_RESET_OFFSET,
        .ResetCount = SM8450_PCIE_RESET_COUNT,
    },
    {
        .Label = "pcie1_phy",
        .Compatible = "qcom,sm8450-qmp-gen4x2-pcie-phy",
        .Base = SM8450_PCIE1_PHY,
        .Size = 0x2000,
        .Lanes = 2,
        .BlockOffsets = { GEN4_SERDES, GEN4_TX, GEN4_RX, GEN4_TX2, GEN4_RX2,
                          GEN4_PCS, GEN4_PCS_MISC },
        .SwReset = QMP_SW_RESET,
        .StartControl = QMP_START,
        .Status = QMP_STATUS,
        .PowerDownControl = QMP_POWER_DOWN,
        .PowerDownControlValue = QMP_SW_PWRDN | QMP_REFCLK_DISABLE,
        .StatusMask = QMP_STATUS_GEN4,
        .InitOffset = ARRAY_SIZE(mGen3PhyInit),
        .InitCount = ARRAY_SIZE(mGen4PhyInit),
        .ClockOffset = SM8450_PCIE1_PHY_CLOCK_OFFSET,
        .ClockCount = SM8450_PCIE_PHY_CLOCK_COUNT,
        .ResetOffset = SM8450_PCIE1_PHY_RESET_OFFSET,
        .ResetCount = SM8450_PCIE_RESET_COUNT,
    },
};

STATIC CONST PcieTargetController mControllers[] = {
    {
        .Label = "pcie0",
        .Compatible = "qcom,pcie-sm8450-pcie0",
        .LinuxConfig = "pcie-qcom",
        .LinuxOps = "qcom,pcie-sm8450-pcie0",
        .Enabled = TRUE,
        /* RC supports 64-bit/DAC DMA; this is independent of BAR windows. */
        .DmaAbove4G = TRUE,
        .Domain = SM8450_PCIE0_DOMAIN,
        /* sm8450-hdk.dts limits the board link to Gen2. */
        .MaxLinkSpeed = 2,
        .BusStart = 0,
        .BusEnd = 0xff,
        .Lanes = 1,
        .RegionOffset = SM8450_PCIE0_REGION_OFFSET,
        .RegionCount = SM8450_PCIE0_REGION_COUNT,
        .RangeOffset = SM8450_PCIE0_RANGE_OFFSET,
        .RangeCount = SM8450_PCIE0_RANGE_COUNT,
        .InterruptOffset = SM8450_PCIE0_INTERRUPT_OFFSET,
        .InterruptCount = SM8450_PCIE_INTERRUPT_COUNT,
        .IntxOffset = SM8450_PCIE0_INTX_OFFSET,
        .IntxCount = SM8450_PCIE_INTX_COUNT,
        .ClockOffset = SM8450_PCIE0_CLOCK_OFFSET,
        .ClockCount = SM8450_PCIE0_CLOCK_COUNT,
        .ResetOffset = SM8450_PCIE0_RESET_OFFSET,
        .ResetCount = SM8450_PCIE_RESET_COUNT,
        .SupplyOffset = SM8450_PCIE0_SUPPLY_OFFSET,
        .SupplyCount = SM8450_PCIE_SUPPLY_COUNT,
        .PowerDomainController = "gcc",
        .PowerDomainId = "pcie_0_gdsc",
        .GpioOffset = SM8450_PCIE0_GPIO_OFFSET,
        .GpioCount = SM8450_PCIE_GPIO_COUNT,
        .IommuMapOffset = SM8450_PCIE0_IOMMU_OFFSET,
        .IommuMapCount = SM8450_PCIE_IOMMU_COUNT,
        .PhyIndex = 0,
        .Interconnect = {
            .Provider = SM8450_ICC_PROVIDER,
            .MemSource = SM8450_ICC_PCIE0_MEM_SRC,
            .MemDestination = SM8450_ICC_PCIE0_MEM_DST,
            .CpuSource = SM8450_ICC_CPU_SRC,
            .CpuDestination = SM8450_ICC_PCIE0_CFG_DST,
            .MemAverage = 0,
            .MemPeak = 250000,
            .CpuAverage = 0,
            .CpuPeak = 1000,
        },
    },
    {
        .Label = "pcie1",
        .Compatible = "qcom,pcie-sm8450-pcie1",
        .LinuxConfig = "pcie-qcom",
        .LinuxOps = "qcom,pcie-sm8450-pcie1",
        .Enabled = TRUE,
        .DmaAbove4G = TRUE,
        .Domain = SM8450_PCIE1_DOMAIN,
        .MaxLinkSpeed = 4,
        .BusStart = 0,
        .BusEnd = 0xff,
        .Lanes = 2,
        .RegionOffset = SM8450_PCIE1_REGION_OFFSET,
        .RegionCount = SM8450_PCIE1_REGION_COUNT,
        .RangeOffset = SM8450_PCIE1_RANGE_OFFSET,
        .RangeCount = SM8450_PCIE1_RANGE_COUNT,
        .InterruptOffset = SM8450_PCIE1_INTERRUPT_OFFSET,
        .InterruptCount = SM8450_PCIE_INTERRUPT_COUNT,
        .IntxOffset = SM8450_PCIE1_INTX_OFFSET,
        .IntxCount = SM8450_PCIE_INTX_COUNT,
        .ClockOffset = SM8450_PCIE1_CLOCK_OFFSET,
        .ClockCount = SM8450_PCIE1_CLOCK_COUNT,
        .ResetOffset = SM8450_PCIE1_RESET_OFFSET,
        .ResetCount = SM8450_PCIE_RESET_COUNT,
        .SupplyOffset = SM8450_PCIE1_SUPPLY_OFFSET,
        .SupplyCount = SM8450_PCIE_SUPPLY_COUNT,
        .PowerDomainController = "gcc",
        .PowerDomainId = "pcie_1_gdsc",
        .GpioOffset = SM8450_PCIE1_GPIO_OFFSET,
        .GpioCount = SM8450_PCIE_GPIO_COUNT,
        .IommuMapOffset = SM8450_PCIE1_IOMMU_OFFSET,
        .IommuMapCount = SM8450_PCIE_IOMMU_COUNT,
        .PhyIndex = 1,
        .Interconnect = {
            .Provider = SM8450_ICC_PROVIDER,
            .MemSource = SM8450_ICC_PCIE1_MEM_SRC,
            .MemDestination = SM8450_ICC_PCIE1_MEM_DST,
            .CpuSource = SM8450_ICC_CPU_SRC,
            .CpuDestination = SM8450_ICC_PCIE1_CFG_DST,
            .MemAverage = 0,
            .MemPeak = 250000,
            .CpuAverage = 0,
            .CpuPeak = 1000,
        },
    },
};

CR_STATIC_ASSERT(ARRAY_SIZE(mRegions) == SM8450_PCIE_REGION_TOTAL,
                 "PCIe region slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mRanges) == SM8450_PCIE_RANGE_TOTAL,
                 "PCIe range slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mInterrupts) == SM8450_PCIE_INTERRUPT_TOTAL,
                 "PCIe interrupt slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mClocks) == SM8450_PCIE_CLOCK_TOTAL,
                 "PCIe clock slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mResets) == SM8450_PCIE_RESET_TOTAL,
                 "PCIe reset slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mSupplies) == SM8450_PCIE_SUPPLY_TOTAL,
                 "PCIe supply slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mGpios) == SM8450_PCIE_GPIO_TOTAL,
                 "PCIe GPIO slices must cover the complete table");
CR_STATIC_ASSERT(ARRAY_SIZE(mIommuMaps) == SM8450_PCIE_IOMMU_TOTAL,
                 "PCIe IOMMU slices must cover the complete table");

STATIC PciePhyInitEntry mAllPhyInit[ARRAY_SIZE(mGen3PhyInit) +
                                     ARRAY_SIZE(mGen4PhyInit)];

STATIC PcieTargetContext mPcieTarget = {
    .Controllers = mControllers,
    .ControllerCount = ARRAY_SIZE(mControllers),
    .Regions = mRegions,
    .RegionCount = ARRAY_SIZE(mRegions),
    .Ranges = mRanges,
    .RangeCount = ARRAY_SIZE(mRanges),
    .Interrupts = mInterrupts,
    .InterruptCount = ARRAY_SIZE(mInterrupts),
    .Clocks = mClocks,
    .ClockCount = ARRAY_SIZE(mClocks),
    .Resets = mResets,
    .ResetCount = ARRAY_SIZE(mResets),
    .Supplies = mSupplies,
    .SupplyCount = ARRAY_SIZE(mSupplies),
    .Gpios = mGpios,
    .GpioCount = ARRAY_SIZE(mGpios),
    .IommuMaps = mIommuMaps,
    .IommuMapCount = ARRAY_SIZE(mIommuMaps),
    .Phys = mPhys,
    .PhyCount = ARRAY_SIZE(mPhys),
    .PhyInit = mAllPhyInit,
    .PhyInitCount = ARRAY_SIZE(mAllPhyInit),
};

STATIC UINT32
TargetPcieRead32(IN VOID *Context, IN UINT64 Address)
{
  UNREFERENCED_PARAMETER(Context);
  return CrMmioRead32((UINTN)Address);
}

STATIC VOID
TargetPcieWrite32(IN VOID *Context, IN UINT64 Address, IN UINT32 Value)
{
  UNREFERENCED_PARAMETER(Context);
  (VOID)CrMmioWrite32((UINTN)Address, Value);
}

STATIC UINT8
TargetPcieRead8(IN VOID *Context, IN UINT64 Address)
{
  UNREFERENCED_PARAMETER(Context);
  return MmioRead8((UINTN)Address);
}

STATIC UINT16
TargetPcieRead16(IN VOID *Context, IN UINT64 Address)
{
  UNREFERENCED_PARAMETER(Context);
  return MmioRead16((UINTN)Address);
}

STATIC VOID
TargetPcieWrite8(IN VOID *Context, IN UINT64 Address, IN UINT8 Value)
{
  UNREFERENCED_PARAMETER(Context);
  (VOID)MmioWrite8((UINTN)Address, Value);
}

STATIC VOID
TargetPcieWrite16(IN VOID *Context, IN UINT64 Address, IN UINT16 Value)
{
  UNREFERENCED_PARAMETER(Context);
  (VOID)MmioWrite16((UINTN)Address, Value);
}

STATIC VOID
TargetPcieDelayUs(IN VOID *Context, IN UINT32 Microseconds)
{
  UNREFERENCED_PARAMETER(Context);
  cr_sleep(Microseconds);
}

/* These placeholders keep the portable callback table structurally complete
 * when a caller uses CrTargetLib on its own.  CranePcieHostLib replaces them
 * with adapters for the already-present MU drivers before PcieLibInit(). */
STATIC CR_STATUS
TargetPcieSetClock(IN VOID *Context, IN CONST PcieTargetClock *Clock,
                   IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Clock);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetReset(IN VOID *Context, IN CONST PcieTargetReset *Reset,
                   IN BOOLEAN Assert)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Reset);
  UNREFERENCED_PARAMETER(Assert);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetPowerDomain(IN VOID *Context,
                         IN CONST PcieTargetController *Controller,
                         IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Controller);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetSupply(IN VOID *Context, IN CONST PcieTargetSupply *Supply,
                    IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Supply);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetInterconnect(IN VOID *Context,
                          IN CONST PcieTargetController *Controller,
                          IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Controller);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetIommu(IN VOID *Context,
                   IN CONST PcieTargetController *Controller,
                   IN BOOLEAN Enable)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Controller);
  UNREFERENCED_PARAMETER(Enable);
  return CR_UNSUPPORTED;
}

STATIC CR_STATUS
TargetPcieSetGpio(IN VOID *Context, IN CONST PcieTargetGpio *Gpio,
                  IN PCIE_GPIO_DIRECTION Direction, IN BOOLEAN Active)
{
  UNREFERENCED_PARAMETER(Context);
  UNREFERENCED_PARAMETER(Gpio);
  UNREFERENCED_PARAMETER(Direction);
  UNREFERENCED_PARAMETER(Active);
  return CR_UNSUPPORTED;
}

PcieTargetContext *
CrTargetGetPcieContext(VOID)
{
  STATIC BOOLEAN Initialized;
  UINTN Index;

  if (!Initialized) {
    for (Index = 0; Index < ARRAY_SIZE(mGen3PhyInit); Index++) {
      mAllPhyInit[Index] = mGen3PhyInit[Index];
    }
    for (Index = 0; Index < ARRAY_SIZE(mGen4PhyInit); Index++) {
      mAllPhyInit[ARRAY_SIZE(mGen3PhyInit) + Index] = mGen4PhyInit[Index];
    }
    Initialized = TRUE;
  }
  return (PcieTargetContext *)&mPcieTarget;
}

CR_STATUS
CrTargetGetPcieIo(OUT struct _PcieIoOps *Io)
{
  if (Io == NULL) {
    return CR_INVALID_PARAMETER;
  }

  cr_memset(Io, 0, sizeof(*Io));

  /* Resource callbacks are replaced by CranePcieHostLib, which owns the
   * corresponding MU drivers.  The explicit stubs keep the ABI valid for
   * callers which inspect the table before installing those adapters. */
  Io->Read32 = TargetPcieRead32;
  Io->Write32 = TargetPcieWrite32;
  Io->Read8 = TargetPcieRead8;
  Io->Read16 = TargetPcieRead16;
  Io->Write8 = TargetPcieWrite8;
  Io->Write16 = TargetPcieWrite16;
  Io->DelayUs = TargetPcieDelayUs;
  Io->SetClock = TargetPcieSetClock;
  Io->SetReset = TargetPcieSetReset;
  Io->SetPowerDomain = TargetPcieSetPowerDomain;
  Io->SetSupply = TargetPcieSetSupply;
  Io->SetInterconnect = TargetPcieSetInterconnect;
  Io->SetIommu = TargetPcieSetIommu;
  Io->SetGpio = TargetPcieSetGpio;
  Io->Context = NULL;
  return CR_SUCCESS;
}
