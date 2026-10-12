/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <Uefi.h>

#define CR_BOOT_DATA_HOB_REVISION 1U
#define CR_BOOT_DATA_FDT_MAX (2U * 1024U * 1024U)
/* Linux bootconfig plus checksum/length/magic and up to three padding bytes. */
#define CR_BOOT_DATA_TAIL_MAX (64U * 1024U + 23U)
#define CR_BOOT_DATA_HOB_GUID \
  {0x1ae145ad,0x2e71,0x46e4,{0x93,0xb4,0x2a,0x79,0x41,0x33,0x07,0xcc}}
extern EFI_GUID gCrBootDataGuid;

/* Describes copies, never permission to read the original ABL buffers.
 * AllocationBase/Size must match the named boot-services-data allocation HOB.
 * SEC excludes this allocation from PHIT's bump allocator before publication.
 * Crane PrePi constructs the HOB list at PHIT.EfiMemoryBottom. After DXE's
 * copy, physical PHIT fields retain their historical values; consumers rebase
 * the checked list length, never dereference EfiEndOfHobList as a live pointer.
 * TailStatus records missing/malformed/inaccessible initrd metadata separately,
 * so DXE can distinguish an absent mode from a rejected bootconfig source.
 * FdtSize/TailSize are exact copied byte counts, not allocation capacities. */
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  EFI_STATUS FdtStatus;
  EFI_STATUS TailStatus;
  UINT64 AllocationBase;
  UINT64 AllocationSize;
  UINT64 FdtOriginal;
  UINT64 FdtCopy;
  UINT64 FdtSize;
  UINT64 TailOriginal;
  UINT64 TailCopy;
  UINT64 TailSize;
} CR_BOOT_DATA_HOB;
