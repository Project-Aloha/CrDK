/* Actual SM8450 target; only DAL protocol lookup is modeled. MIT */
#pragma once
#include <Library/CrTargetSdLib.h>
#include "../Target/Sm8450/CrSdTarget.c"
STATIC CONST CR_TARGET_SD_CONTEXT *mTestSdTarget;
STATIC BOOLEAN mTestSdMissing;
CONST CR_TARGET_SD_CONTEXT *CrDalGetSdContext(VOID) {
  return mTestSdMissing?NULL:(mTestSdTarget?mTestSdTarget:CrTargetGetSdContext());
}
