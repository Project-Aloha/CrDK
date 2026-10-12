/** Immutable current-boot APPS RAM grant query. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>

#define EFI_CR_RAM_RANGE_PROTOCOL_REVISION 0x00010000ULL
#define EFI_CR_RAM_RANGE_PROTOCOL_GUID \
  { 0x7ccf457e, 0x9ed1, 0x4b4f, { 0xb8, 0xd7, 0x37, 0x17, 0x59, 0x62, 0xc6, 0x40 } }
extern EFI_GUID gEfiCrRamRangeProtocolGuid;
typedef struct _EFI_CR_RAM_RANGE_PROTOCOL EFI_CR_RAM_RANGE_PROTOCOL;
/* Nonzero page-aligned range, wholly within one normalized current-boot bank's
 * authoritative AvailableLength, with READWRITE permission. Physical Length
 * is never a substitute, including when AvailableLength is zero. Exact
 * EFI_SUCCESS proves inventory coverage only; it grants no mapping, cache,
 * execution, allocation or DMA access. No allocations or physical reads. */
typedef EFI_STATUS (EFIAPI *EFI_CR_RAM_RANGE_CHECK)(
  EFI_CR_RAM_RANGE_PROTOCOL *This, UINT64 Base, UINT64 Bytes);
struct _EFI_CR_RAM_RANGE_PROTOCOL {
  UINT64 Revision;
  EFI_CR_RAM_RANGE_CHECK CheckAppsReadWriteRange;
};
