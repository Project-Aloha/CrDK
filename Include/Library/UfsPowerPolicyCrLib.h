/* SPDX-License-Identifier: MIT */
#pragma once
#include <Library/CrTargetUfsLib.h>
#include <Library/UfsPowerFdtCrLib.h>

#define CR_UFS_POWER_HOB_VERSION 1U
typedef struct {
  UINT32 Version, Size;
  UINT64 CaptureStatus;
  CR_UFS_POWER_FDT Info;
} CR_UFS_POWER_HOB;
extern EFI_GUID gCrUfsPowerInfoGuid;

typedef struct {
  UINT32 Count;
  CR_TARGET_UFS_RAIL Rails[CR_TARGET_UFS_MAX_RAILS];
  UINT32 Sources[CR_TARGET_UFS_MAX_RAILS]; /* 1=DT init/fixed, 2=board preference */
  UINT32 InheritedRoles;
  UINT32 RetainedRailMask;
  CONST CHAR8 *RetainedEnableSupply;
} CR_UFS_POWER_RESOLVED;

/* Pure qualification; output unchanged on failure. The returned rail name
 * pointers belong to the immutable target policy, never to a HOB or FDT. */
EFI_STATUS CrUfsPowerResolve(CONST CR_TARGET_UFS_CONTEXT *Target,
                            CONST CR_UFS_POWER_FDT *Info,
                            CR_UFS_POWER_RESOLVED *Result);
