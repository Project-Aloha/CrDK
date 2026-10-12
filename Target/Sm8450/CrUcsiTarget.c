/* Existing native HDK8450 transport policy, now explicitly board-selected.
 * Preserve current Crane adapter contracts. SPDX-License-Identifier: MIT */
#include <Library/UcsiTargetLib.h>
STATIC CONST CR_UCSI_TARGET mTarget={0x8450,8450,0xed18000,261,91,TRUE,FALSE,"ldob2",3072,7};
CONST CR_UCSI_TARGET *CrUcsiGetTarget(VOID){return &mTarget;}
