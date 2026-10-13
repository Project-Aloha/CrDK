/**
  Qualcomm SMMU target description used by the platform-independent DXE
  adapter.  A target implementation must describe the already mapped SMMU
  window; the adapter never guesses a base address from a PCI controller.

  SPDX-License-Identifier: MIT
**/
#ifndef CR_TARGET_SMMU_LIB_H_
#define CR_TARGET_SMMU_LIB_H_

#include <oskal/cr_types.h>
#include <Library/smmu.h>

typedef struct {
  UINT64 Base;
  UINT64 Size;
  UINT16 Sid;
} CrTargetSmmuDmaDevice;

typedef struct {
  UINT64 Base;
  UINT64 Size;
  UINT16 SegmentCount;
  UINT16 MaximumDomains;
  /**
    Optional stream IDs which this target's cold-init path owns.  The DXE
    adapter uses this list as an allow-list for EFI_SMMU_CR_PROTOCOL.Attach();
    each controller callback supplies the real segment/BDF while its endpoint
    remains in reset.  An empty list allows every stream ID.
  **/
  CONST UINT16 *DefaultStreamIds;
  UINT16       DefaultStreamCount;
  /**
    TRUE only when the platform has independent evidence that an already
    installed IOMMU producer owns every DefaultStreamIds entry.  Merely
    locating EDKII_IOMMU_PROTOCOL is not sufficient proof of stream setup.  If
    an external producer is present while this is FALSE, the adapter refuses
    to initialize so PCIe cannot be exposed without an ownership contract.
  **/
  BOOLEAN      ExternalIoMmuOwnsPcieStreams;
  /** Optional per-client ACTLR settings from the target's public DT. Only
      newly allocated Crane banks are modified; secure/global cache locks
      remain the responsibility of the earlier firmware handoff. **/
  CONST SmmuActlrSetting *ActlrSettings;
  UINTN                  ActlrSettingCount;
  /** Bits which this target's SMMU may legally drop on SCTLR readback. */
  UINT32                 SctlrReadbackIgnoreMask;
  CONST CrTargetSmmuDmaDevice *DmaDevices;
  UINTN                       DmaDeviceCount;
  /** Exact SID subset of DefaultStreamIds whose target describes hardware
      coherent DMA. The adapter selects coherent stage-1 memory attributes
      only for newly acquired domains in this list. Empty preserves the
      legacy noncached policy; no SID mask expansion is implied. This is
      native translation policy, not an ACPI _CCA or OS handoff grant. **/
  CONST UINT16 *CoherentStreamIds;
  UINT16       CoherentStreamCount;
} CrTargetSmmuContext;

CrTargetSmmuContext *
CrTargetGetSmmuContext(
  VOID
  );

#endif
