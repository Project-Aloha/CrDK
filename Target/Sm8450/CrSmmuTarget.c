/**
  SM8450 SMMU target data.

  The SMMU is enabled by the earlier firmware hand-off on production boards;
  the DXE adapter probes it without resetting firmware-owned streams.  The
  target data therefore contains only the architectural MMIO window and the
  segment/domain limit.  PCIe stream IDs remain board data and are supplied to
  the management Attach() call by cold-init.

  SPDX-License-Identifier: MIT
**/
#include <Library/CrTargetSmmuLib.h>

/*
 * Linux sm8450.dtsi maps PCIe0 to 1c00/1c01 and PCIe1 to 1c80/1c81.  Keep
 * the list here, next to the SMMU window, so the DXE adapter and the cold
 * PCIe/UFS/display paths consume the same stream contract instead of inventing IDs
 * from a BDF.  PCIe endpoints are held in PERST while attached; UFS uses
 * the DT SID 0xe0 directly.
 */
/* Linux sm8450.dtsi assigns GPI1 ring SID 0x56, QUP1 payload SID 0x43,
 * UFS SID 0xe0 and MDSS SID/mask 0x2800/0x402.
 * Expanding the ignored mask bits yields four exact MDSS SIDs. */
STATIC CONST UINT16 mSm8450StreamIds[] = {
  0x0000, /* Linux sm8450.dtsi: DWC3 USB3_0, mask 0 */
  0x1C00, 0x1C01, 0x1C80, 0x1C81, 0x0056, 0x0043, 0x00E0,
  0x2800, 0x2802, 0x2C00, 0x2C02
};

/* OnePlus SM8450 DT 877203a7cf9b79973d44a9db716490f95d820110:
 * waipio-pcie.dtsi marks both root ports dma-coherent and supplies the exact
 * two iommu-map SIDs for each port. waipio.dtsi marks UFS SID 0xe0 coherent.
 *
 * UFS keeps coherent DMA from native initialization through OS handoff.
 * CrUfsTarget requests CPU WB common buffers; CrSmmuAllocateBuffer and the
 * grant paths enforce that these buffers reach only coherent domains. This
 * pairs Android dma_pgprot's cacheable CPU mapping with IOMMU_CACHE.
 *
 * At EBS UFS stops and its UEFI mappings are revoked before switching to
 * coherent physical DMA for Windows _CCA=1. No PCIe ACPI roots or EBS bypass
 * follow from this list; USB, QUP/GPI and MDSS keep their existing policy.
 */
STATIC CONST UINT16 mSm8450CoherentStreamIds[] = {
  0x1C00, 0x1C01, 0x1C80, 0x1C81, 0x00E0
};

/* Public OnePlus Waipio DT, revision 877203a7cf9b79973d44a9db716490f95d820110,
 * msm-arm-smmu-waipio.dtsi: qcom,actlr matches the MDSS SID/mask to value 1.
 * Linux arm-smmu-qcom.c independently selects CMTLB without prefetch for
 * qcom,sm8450-mdss. No override in that DT matches our UFS/GPI/PCIe SIDs. */
STATIC CONST SmmuActlrSetting mSm8450ActlrSettings[] = {
  { 0x2800, 0x0402, 0x00000001 }
};

STATIC CONST CrTargetSmmuDmaDevice mSm8450DmaDevices[] = {
  { 0x0A600000, 0x00100000, 0x0000 }
};

STATIC CrTargetSmmuContext mSm8450SmmuContext = {
  .Base           = 0x15000000ULL,
  .Size           = 0x00100000ULL,
  .SegmentCount   = 2,
  .MaximumDomains = 16,
  .DefaultStreamIds   = mSm8450StreamIds,
  .DefaultStreamCount = (UINT16)(sizeof (mSm8450StreamIds) /
                                 sizeof (mSm8450StreamIds[0])),
  /* HALIOMMU protocol presence alone does not prove these PCIe SIDs are live. */
  .ExternalIoMmuOwnsPcieStreams = FALSE,
  .ActlrSettings = mSm8450ActlrSettings,
  .ActlrSettingCount = sizeof (mSm8450ActlrSettings) /
                       sizeof (mSm8450ActlrSettings[0]),
  /* SM8450 QSMMUv500 clears SCTLR.S1_ASIDPNE (bit 12) on readback while
   * retaining the AArch64 stage-1 configuration. Linux accepts this hardware
   * behavior; all other SCTLR bits remain exact-readback requirements. */
  .SctlrReadbackIgnoreMask = (1U << 12),
  .DmaDevices = mSm8450DmaDevices,
  .DmaDeviceCount = sizeof(mSm8450DmaDevices) / sizeof(mSm8450DmaDevices[0]),
  .CoherentStreamIds = mSm8450CoherentStreamIds,
  .CoherentStreamCount = (UINT16)(sizeof (mSm8450CoherentStreamIds) /
                                  sizeof (mSm8450CoherentStreamIds[0]))
};

CrTargetSmmuContext *
CrTargetGetSmmuContext (
  VOID
  )
{
  return &mSm8450SmmuContext;
}
