/** A730 GMU/SQE format inspection; no load or authentication authority.
    SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_ADRENO_FIRMWARE_PLAN_VERSION 1U
#define CR_ADRENO_FIRMWARE_PROFILE_A730_GEN7 1U
/* Explicit parser bounds, not firmware or hardware capacity measurements. */
#define CR_ADRENO_FIRMWARE_MAX_BYTES (16U * 1024U * 1024U)
#define CR_ADRENO_GMU_MAX_RECORDS 128U
#define CR_ADRENO_GMU_HEADER_BYTES 16U

#define CR_ADRENO_GMU_TYPE_DATA 0U
#define CR_ADRENO_GMU_TYPE_PREALLOC 1U
#define CR_ADRENO_GMU_TYPE_CORE_VERSION 2U
#define CR_ADRENO_GMU_TYPE_CORE_DEV_VERSION 3U
#define CR_ADRENO_GMU_TYPE_POWER_VERSION 4U
#define CR_ADRENO_GMU_TYPE_POWER_DEV_VERSION 5U
#define CR_ADRENO_GMU_TYPE_HFI_VERSION 6U
#define CR_ADRENO_GMU_TYPE_PERSISTENT_PREALLOC 7U

typedef enum {
  CrAdrenoGmuRegionNone = 0,
  CrAdrenoGmuRegionItcm = 1,
  CrAdrenoGmuRegionDtcm = 2,
  CrAdrenoGmuRegionCache = 3,
  CrAdrenoGmuRegionUncached = 4
} CR_ADRENO_GMU_REGION;

typedef enum {
  CrAdrenoGmuRecordZeroData = 0,
  CrAdrenoGmuRecordData = 1,
  CrAdrenoGmuRecordPreallocation = 2,
  CrAdrenoGmuRecordVersion = 3
} CR_ADRENO_GMU_ACTION;

typedef struct {
  UINT32 HeaderOffset;
  UINT32 PayloadOffset;
  UINT32 Destination;
  /* Payload size for data; requested value for preallocation; zero otherwise. */
  UINT32 Bytes;
  UINT32 Type;
  UINT32 Value;
  UINT32 Action;
  UINT32 Region;
} CR_ADRENO_GMU_RECORD;

typedef struct {
  UINT32 Version;
  UINT32 Profile;
  UINT32 ImageBytes;
  UINT32 RecordCount;
  UINT32 DataCount;
  UINT32 PreallocationCount;
  /* Bits 0..4 and corresponding raw values for metadata types 2..6. */
  UINT32 VersionMask;
  UINT32 TotalDataBytes;
  UINT32 Versions[5];
  UINT32 Reserved[3];
  CR_ADRENO_GMU_RECORD Records[CR_ADRENO_GMU_MAX_RECORDS];
} CR_ADRENO_GMU_PLAN;

#define CR_ADRENO_SQE_PATCHED_VERSION 1U
typedef struct {
  UINT32 Version;
  UINT32 PrefixWord;
  UINT32 CodeOffset;
  UINT32 CodeBytes;
  UINT32 CodeWords;
  UINT32 RawCodeVersion;
  UINT32 CodeVersion;
  UINT32 Flags;
} CR_ADRENO_SQE_PLAN;

CR_STATIC_ASSERT(sizeof(CR_ADRENO_GMU_RECORD) == 32, "GMU record layout");
CR_STATIC_ASSERT(sizeof(CR_ADRENO_GMU_PLAN) == 4160, "GMU plan layout");
CR_STATIC_ASSERT(sizeof(CR_ADRENO_SQE_PLAN) == 32, "SQE plan layout");

/* Inspect a caller-owned immutable byte copy, retained for the entire call.
   The fixed A730/Gen7 profile has ITCM[0,4000), DTCM[10004000,10008000),
   cache[4000,1000000), and uncached[60000000,80000000). The Android extended
   uncached range is not part of this first mainline profile.
   Whole data and preallocation ranges must fit one region without wrapping.
   Data address/length are DWORD-aligned; nonzero payload Type/Value are opaque
   and retained (released A730 firmware uses 0xefbeadde). Zero-size metadata
   precedes all data and remains typed. Unknown metadata types, conflicting versions, overlapping
   data and incompatible overlapping preallocations are rejected. Exact repeated
   preallocation requirements are preserved; version duplicates must agree.
   At least one data block is required. No padding beyond complete records.
   Plan must be DWORD-aligned and disjoint from input. Every failure leaves it
   unchanged. No allocation or large temporary plan is placed on the stack.

   Records are byte requirements, not a grant to write their destinations. The
   future loader must prove exact live TCM views and backing for every non-TCM
   data/preallocation record, including persistent lifetimes, before loading.
   This does not authenticate firmware, select compatible bytes, create a map,
   or claim the native/ABL/GMU writer is stopped. */
CR_STATUS CrAdrenoGmuInspect(CONST VOID *Image, UINTN ImageBytes,
                             CR_ADRENO_GMU_PLAN *Plan);

/* SQE is one prefix DWORD followed by nonempty aligned code. CodeVersion is
   code word0 (file word1); patched nibble0xa also reads code word2/file word3.
   PrefixWord is retained without inventing its semantics. The code backing
   must be GPU-read-only in a future actual loader. No A630/A650 version gate,
   authentication, signature or hardware readiness is inferred for A730.
   Stable input, alignment/disjoint output and failure-retention rules as above. */
CR_STATUS CrAdrenoSqeInspect(CONST VOID *Image, UINTN ImageBytes,
                             CR_ADRENO_SQE_PLAN *Plan);
