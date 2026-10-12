/* Producer-private, pinned display ownership and buffer journals.
 * SPDX-License-Identifier: MIT */
#include <Library/SmmuDisplayOwnershipCrLib.h>
typedef struct _CR_SMMU_DISPLAY_BUFFER CR_SMMU_DISPLAY_BUFFER;
typedef struct {
  CR_SMMU_DISPLAY_GROUP_CORE Core;
  CONST CR_MDSS_TARGET *Target;
  CR_MDSS_TARGET TargetIdentity;
  CONST CrTargetSmmuContext *SmmuTarget;
  EFI_SMMU_CR_DISPLAY_GROUP *Client;
  EFI_SMMU_CR_DISPLAY_VERIFY_STOPPED VerifyStopped;
  VOID *Context;
  UINT64 Generation, OwnerGeneration;
  UINT32 NextAttach;
  BOOLEAN Active, Busy, Complete, Failed, RetirementCheck;
  EFI_STATUS LastStatus;
} CR_SMMU_DISPLAY_NATIVE;
struct _CR_SMMU_DISPLAY_BUFFER {
  EFI_SMMU_CR_DISPLAY_LEASE *Client;
  UINT64 Generation, GroupGeneration;
  EFI_PHYSICAL_ADDRESS Address, Iova;
  UINTN Bytes, Pages;
  UINT64 OriginalAttributes;
  CR_SMMU_MAPPING *Mapping;
  EFI_STATUS LastStatus;
  UINT32 Kind, GrantedMask;
  BOOLEAN InUse, Busy, Allocated, BorrowedFrame, AttributesTouched;
  BOOLEAN CpuReady, CleanupOnly, Ready, AllocationUncertain;
};
STATIC CR_SMMU_DISPLAY_NATIVE mDisplayGroup;
STATIC CR_SMMU_DISPLAY_BUFFER mDisplayBuffers[2];
STATIC UINT64 mDisplayGeneration, mDisplayBufferGeneration;
STATIC BOOLEAN DisplaySid(UINT16 Sid);
STATIC BOOLEAN DisplayMappingProtected(CONST CR_SMMU_MAPPING *Mapping);
STATIC BOOLEAN DisplayAllocationOverlap(UINT64 Address,UINT64 Bytes);
STATIC EFI_STATUS DisplayRoutesLocked(BOOLEAN Empty);
