/** Native on-chip controller DMA identity. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>

#define EFI_DMA_IDENTITY_CR_PROTOCOL_GUID \
  { 0x7c18b43c, 0xa541, 0x47c1, { 0xb7, 0xb5, 0x68, 0xd0, 0x13, 0x9c, 0xfa, 0x42 } }
#define EFI_DMA_IDENTITY_CR_PROTOCOL_REVISION 0x00010000ULL
extern EFI_GUID gEfiDmaIdentityCrProtocolGuid;

/* The SMMU provider verifies this tuple against immutable target data and an
   attached on-chip domain. It never derives a SID from synthetic PCI IDs.
   Keep this protocol installed until every controller mapping is revoked. */
typedef struct {
  UINT64 Revision;
  UINT64 IommuBase;
  UINT64 ControllerBase;
  UINT64 ControllerSize;
  UINT16 Sid;
} EFI_DMA_IDENTITY_CR_PROTOCOL;
