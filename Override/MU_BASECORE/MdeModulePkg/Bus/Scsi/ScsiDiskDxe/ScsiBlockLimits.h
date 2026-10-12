/** @file
  Block Limits VPD wire layout for the Crane ScsiDisk adapter.

  Derived from MdePkg/Include/IndustryStandard/Scsi.h.
  Copyright (c) 2006 - 2020, Intel Corporation. All rights reserved.<BR>
  SPDX-License-Identifier: BSD-2-Clause-Patent

  SBC Block Limits page B0 has a 60-byte payload plus its 4-byte header.
  Maximum WRITE SAME length occupies bytes 36..43 (eight bytes). The pinned
  MU header omits the upper four bytes. Keep the corrected wire layout local
  so this driver requests the full page without modifying MU dependencies.
**/
#ifndef CR_SCSI_BLOCK_LIMITS_H_
#define CR_SCSI_BLOCK_LIMITS_H_

#include <Uefi.h>

#pragma pack(1)
typedef struct {
  UINT8    Peripheral_Type      : 5;
  UINT8    Peripheral_Qualifier : 3;
  UINT8    PageCode;
  UINT8    PageLength2;
  UINT8    PageLength1;
  UINT8    WriteSameNonZero     : 1;
  UINT8    Reserved_4           : 7;
  UINT8    MaximumCompareAndWriteLength;
  UINT8    OptimalTransferLengthGranularity2;
  UINT8    OptimalTransferLengthGranularity1;
  UINT8    MaximumTransferLength4;
  UINT8    MaximumTransferLength3;
  UINT8    MaximumTransferLength2;
  UINT8    MaximumTransferLength1;
  UINT8    OptimalTransferLength4;
  UINT8    OptimalTransferLength3;
  UINT8    OptimalTransferLength2;
  UINT8    OptimalTransferLength1;
  UINT8    MaximumPrefetchXdreadXdwriteTransferLength4;
  UINT8    MaximumPrefetchXdreadXdwriteTransferLength3;
  UINT8    MaximumPrefetchXdreadXdwriteTransferLength2;
  UINT8    MaximumPrefetchXdreadXdwriteTransferLength1;
  UINT8    MaximumUnmapLbaCount4;
  UINT8    MaximumUnmapLbaCount3;
  UINT8    MaximumUnmapLbaCount2;
  UINT8    MaximumUnmapLbaCount1;
  UINT8    MaximumUnmapBlockDescriptorCount4;
  UINT8    MaximumUnmapBlockDescriptorCount3;
  UINT8    MaximumUnmapBlockDescriptorCount2;
  UINT8    MaximumUnmapBlockDescriptorCount1;
  UINT8    OptimalUnmapGranularity4;
  UINT8    OptimalUnmapGranularity3;
  UINT8    OptimalUnmapGranularity2;
  UINT8    OptimalUnmapGranularity1;
  UINT8    UnmapGranularityAlignment4     : 7;
  UINT8    UnmapGranularityAlignmentValid : 1;
  UINT8    UnmapGranularityAlignment3;
  UINT8    UnmapGranularityAlignment2;
  UINT8    UnmapGranularityAlignment1;
  UINT8    MaximumWriteSameLength8;
  UINT8    MaximumWriteSameLength7;
  UINT8    MaximumWriteSameLength6;
  UINT8    MaximumWriteSameLength5;
  UINT8    MaximumWriteSameLength4;
  UINT8    MaximumWriteSameLength3;
  UINT8    MaximumWriteSameLength2;
  UINT8    MaximumWriteSameLength1;
  UINT8    MaximumAtomicTransferLength4;
  UINT8    MaximumAtomicTransferLength3;
  UINT8    MaximumAtomicTransferLength2;
  UINT8    MaximumAtomicTransferLength1;
  UINT8    AtomicAlignment4;
  UINT8    AtomicAlignment3;
  UINT8    AtomicAlignment2;
  UINT8    AtomicAlignment1;
  UINT8    AtomicTransferLengthGranularity4;
  UINT8    AtomicTransferLengthGranularity3;
  UINT8    AtomicTransferLengthGranularity2;
  UINT8    AtomicTransferLengthGranularity1;
  UINT8    MaximumAtomicTransferLengthWithAtomicBoundary4;
  UINT8    MaximumAtomicTransferLengthWithAtomicBoundary3;
  UINT8    MaximumAtomicTransferLengthWithAtomicBoundary2;
  UINT8    MaximumAtomicTransferLengthWithAtomicBoundary1;
  UINT8    MaximumAtomicBoundarySize4;
  UINT8    MaximumAtomicBoundarySize3;
  UINT8    MaximumAtomicBoundarySize2;
  UINT8    MaximumAtomicBoundarySize1;
} CR_SCSI_BLOCK_LIMITS_VPD_PAGE;
#pragma pack()

STATIC_ASSERT (sizeof (CR_SCSI_BLOCK_LIMITS_VPD_PAGE) == 64, "Block Limits VPD size");
STATIC_ASSERT (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumWriteSameLength4) == 40,
               "WRITE SAME low word offset");
STATIC_ASSERT (OFFSET_OF (CR_SCSI_BLOCK_LIMITS_VPD_PAGE, MaximumAtomicTransferLength4) == 44,
               "Atomic transfer offset");
#endif
