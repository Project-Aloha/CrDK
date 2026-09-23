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
 * PCIe path consume the same stream contract instead of inventing IDs from a
 * BDF.  The endpoint is held in PERST while these entries are attached.
 */
STATIC CONST UINT16 mSm8450PcieStreamIds[] = {
  0x1C00, 0x1C01, 0x1C80, 0x1C81
};

STATIC CrTargetSmmuContext mSm8450SmmuContext = {
  .Base           = 0x15000000ULL,
  .Size           = 0x00100000ULL,
  .SegmentCount   = 2,
  .MaximumDomains = 16,
  .DefaultStreamIds   = mSm8450PcieStreamIds,
  .DefaultStreamCount = (UINT16)(sizeof (mSm8450PcieStreamIds) /
                                 sizeof (mSm8450PcieStreamIds[0])),
  /* HALIOMMU protocol presence alone does not prove these PCIe SIDs are live. */
  .ExternalIoMmuOwnsPcieStreams = FALSE
};

CrTargetSmmuContext *
CrTargetGetSmmuContext (
  VOID
  )
{
  return &mSm8450SmmuContext;
}
