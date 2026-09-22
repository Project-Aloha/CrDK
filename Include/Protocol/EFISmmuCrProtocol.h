/**
  Crane SMMU management protocol.

  The protocol is deliberately separate from EDKII_IOMMU_PROTOCOL.  PCIe
  cold-init owns stream quiescing and calls Attach() before it releases PERST
  or enables bus mastering.  BlockSegment() is the shutdown hand-off and must
  be called after the PCIe endpoint has stopped issuing DMA.

  SPDX-License-Identifier: MIT
**/
#ifndef EFI_SMMU_CR_PROTOCOL_H_
#define EFI_SMMU_CR_PROTOCOL_H_

#include <Uefi.h>

#define EFI_SMMU_CR_PROTOCOL_GUID                                             \
  { 0x4c0eced0, 0x2f84, 0x4e6f, { 0xa3, 0xf0, 0x20, 0x1f, 0xa9, 0x55, 0x4e, 0x31 } }

#define EFI_SMMU_CR_PROTOCOL_REVISION  0x00010000ULL

extern EFI_GUID  gEfiSmmuCrProtocolGuid;

typedef struct _EFI_SMMU_CR_PROTOCOL EFI_SMMU_CR_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *EFI_SMMU_CR_ATTACH)(
  IN EFI_SMMU_CR_PROTOCOL  *This,
  IN UINT16                 Segment,
  IN UINT16                 Bdf,
  IN UINT16                 Sid
  );

typedef
EFI_STATUS
(EFIAPI *EFI_SMMU_CR_BLOCK_SEGMENT)(
  IN EFI_SMMU_CR_PROTOCOL  *This,
  IN UINT16                 Segment
  );

struct _EFI_SMMU_CR_PROTOCOL {
  UINT64                    Revision;
  EFI_SMMU_CR_ATTACH        Attach;
  EFI_SMMU_CR_BLOCK_SEGMENT BlockSegment;
};

#endif
