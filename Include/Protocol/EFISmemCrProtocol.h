/** Host-scoped SMEM API for native interprocessor transports. SPDX-License-Identifier: MIT */
#pragma once
#include <Uefi.h>

#define EFI_SMEM_CR_PROTOCOL_REVISION  0x00010000ULL
#define EFI_SMEM_CR_PROTOCOL_GUID \
  { 0xe891c8e7, 0xc0c4, 0x4c82, { 0xa8, 0x03, 0xd3, 0x9d, 0x72, 0x71, 0x8c, 0x3d } }
extern EFI_GUID gEfiSmemCrProtocolGuid;
typedef struct _EFI_SMEM_CR_PROTOCOL EFI_SMEM_CR_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SMEM_CR_GET_ITEM)(
  IN EFI_SMEM_CR_PROTOCOL *This, IN UINT32 Host, IN UINT32 Item,
  OUT UINT32 *Size, OUT VOID **Address);
/* Existing allocations return EFI_ALREADY_STARTED, with Address cleared.
   The caller must GetItem and validate the published size before use. */
typedef EFI_STATUS (EFIAPI *EFI_SMEM_CR_ALLOCATE_ITEM)(
  IN EFI_SMEM_CR_PROTOCOL *This, IN UINT32 Host, IN UINT32 Item,
  IN UINT32 Size, OUT VOID **Address);

struct _EFI_SMEM_CR_PROTOCOL {
  UINT64 Revision;
  EFI_SMEM_CR_GET_ITEM GetItem;
  EFI_SMEM_CR_ALLOCATE_ITEM AllocateItem;
};
