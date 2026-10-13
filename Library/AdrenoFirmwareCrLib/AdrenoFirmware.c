// SPDX-License-Identifier: GPL-2.0-only
/* A730 format decoder based on Linux a6xx_gmu/adreno_gpu and Qualcomm Gen7
 * gmu_block_header metadata definitions. Original checked parser implementation;
 * source provenance and preserved upstream notices are recorded in README.md.
 */
#include <Library/AdrenoFirmwareCrLib.h>
#include <oskal/cr_string.h>

STATIC UINT32 Le32(CONST UINT8 *P) {
  return (UINT32)P[0] | ((UINT32)P[1] << 8) |
         ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}

STATIC BOOLEAN Span(CONST VOID *P, UINTN N, UINTN *End) {
  UINTN B = (UINTN)P;
  if (P == NULL || N == 0 || B > (UINTN)-1 - N) return FALSE;
  *End = B + N;
  return TRUE;
}

STATIC BOOLEAN Separate(CONST VOID *Image, UINTN N, CONST VOID *Plan, UINTN S) {
  UINTN IEnd, PEnd;
  return Span(Image, N, &IEnd) && Span(Plan, S, &PEnd) &&
         (((UINTN)Plan & 3U) == 0) &&
         !((UINTN)Image < PEnd && (UINTN)Plan < IEnd);
}

STATIC UINT32 Region(UINT32 Address, UINT32 N) {
  UINT64 A = Address, End = A + N;
  if (N == 0) return CrAdrenoGmuRegionNone;
  if (A < 0x4000ULL && End <= 0x4000ULL) return CrAdrenoGmuRegionItcm;
  if (A >= 0x10004000ULL && End <= 0x10008000ULL)
    return CrAdrenoGmuRegionDtcm;
  if (A >= 0x4000ULL && End <= 0x1000000ULL)
    return CrAdrenoGmuRegionCache;
  if (A >= 0x60000000ULL && End <= 0x80000000ULL)
    return CrAdrenoGmuRegionUncached;
  return CrAdrenoGmuRegionNone;
}

STATIC BOOLEAN Overlap(UINT32 A, UINT32 AN, UINT32 B, UINT32 BN) {
  return (UINT64)A < (UINT64)B + BN && (UINT64)B < (UINT64)A + AN;
}

STATIC VOID Decode(CONST UINT8 *P, UINT32 Offset, CR_ADRENO_GMU_RECORD *R) {
  R->HeaderOffset = Offset;
  R->PayloadOffset = Offset + CR_ADRENO_GMU_HEADER_BYTES;
  R->Destination = Le32(P);
  R->Bytes = Le32(P + 4);
  R->Type = Le32(P + 8);
  R->Value = Le32(P + 12);
  R->Action = CrAdrenoGmuRecordZeroData;
  R->Region = CrAdrenoGmuRegionNone;
  if (R->Bytes != 0) R->Action = CrAdrenoGmuRecordData;
  else if (R->Type == CR_ADRENO_GMU_TYPE_PREALLOC ||
           R->Type == CR_ADRENO_GMU_TYPE_PERSISTENT_PREALLOC) {
    R->Action = CrAdrenoGmuRecordPreallocation;
    R->Bytes = R->Value;
  } else if (R->Type >= CR_ADRENO_GMU_TYPE_CORE_VERSION &&
             R->Type <= CR_ADRENO_GMU_TYPE_HFI_VERSION)
    R->Action = CrAdrenoGmuRecordVersion;
  if (R->Action == CrAdrenoGmuRecordData ||
      R->Action == CrAdrenoGmuRecordPreallocation)
    R->Region = Region(R->Destination, R->Bytes);
}

CR_STATUS CrAdrenoGmuInspect(CONST VOID *Image, UINTN ImageBytes,
                             CR_ADRENO_GMU_PLAN *Plan) {
  if (!Separate(Image, ImageBytes, Plan, sizeof(*Plan)))
    return CR_INVALID_PARAMETER;
  if (ImageBytes < CR_ADRENO_GMU_HEADER_BYTES ||
      ImageBytes > CR_ADRENO_FIRMWARE_MAX_BYTES) return CR_BAD_BUFFER_SIZE;
  CONST UINT8 *P = (CONST UINT8 *)Image;
  UINT32 Offset = 0, Count = 0, DataCount = 0;
  /* Validate every byte/range before writing any part of the caller plan. */
  while (Offset < ImageBytes) {
    if (ImageBytes - Offset < CR_ADRENO_GMU_HEADER_BYTES)
      return CR_BAD_BUFFER_SIZE;
    if (Count == CR_ADRENO_GMU_MAX_RECORDS) return CR_OUT_OF_RESOURCES;
    CR_ADRENO_GMU_RECORD R;
    Decode(P + Offset, Offset, &R);
    UINT32 WireBytes = Le32(P + Offset + 4);
    if (WireBytes > ImageBytes - R.PayloadOffset) return CR_BAD_BUFFER_SIZE;
    /* Mainline and Android dispatch nonzero blocks by size/address. Released
       A730 v4.00.07 uses opaque type/value 0xefbeadde in every data block. */
    if (WireBytes == 0 && R.Type > CR_ADRENO_GMU_TYPE_PERSISTENT_PREALLOC)
      return CR_UNSUPPORTED;
    if (WireBytes == 0 && DataCount != 0)
      return CR_COMPROMISED_DATA;
    if (R.Action == CrAdrenoGmuRecordData ||
        R.Action == CrAdrenoGmuRecordPreallocation) {
      if (R.Region == CrAdrenoGmuRegionNone) return CR_COMPROMISED_DATA;
      if (R.Action == CrAdrenoGmuRecordData &&
          ((R.Destination | R.Bytes) & 3U) != 0) return CR_COMPROMISED_DATA;
    }
    /* The preceding records are already framing-checked. A bounded scan
       avoids retaining a 4KiB temporary plan on a kernel/firmware stack. */
    UINT32 Previous = 0;
    while (Previous < Offset) {
      CR_ADRENO_GMU_RECORD Old;
      Decode(P + Previous, Previous, &Old);
      if (R.Action == CrAdrenoGmuRecordVersion &&
          Old.Action == CrAdrenoGmuRecordVersion && R.Type == Old.Type &&
          R.Value != Old.Value) return CR_COMPROMISED_DATA;
      if (R.Action == CrAdrenoGmuRecordData &&
          Old.Action == CrAdrenoGmuRecordData &&
          Overlap(R.Destination, R.Bytes, Old.Destination, Old.Bytes))
        return CR_COMPROMISED_DATA;
      if (R.Action == CrAdrenoGmuRecordPreallocation &&
          Old.Action == CrAdrenoGmuRecordPreallocation &&
          Overlap(R.Destination, R.Bytes, Old.Destination, Old.Bytes) &&
          (R.Destination != Old.Destination || R.Bytes != Old.Bytes ||
           R.Type != Old.Type)) return CR_COMPROMISED_DATA;
      Previous += CR_ADRENO_GMU_HEADER_BYTES + Le32(P + Previous + 4);
    }
    if (WireBytes != 0) ++DataCount;
    ++Count;
    Offset = R.PayloadOffset + WireBytes;
  }
  if (DataCount == 0) return CR_NOT_FOUND;
  cr_memset(Plan, 0, sizeof(*Plan));
  Plan->Version = CR_ADRENO_FIRMWARE_PLAN_VERSION;
  Plan->Profile = CR_ADRENO_FIRMWARE_PROFILE_A730_GEN7;
  Plan->ImageBytes = (UINT32)ImageBytes;
  Plan->RecordCount = Count;
  Offset = 0;
  for (UINT32 I = 0; I < Count; ++I) {
    CR_ADRENO_GMU_RECORD *R = &Plan->Records[I];
    Decode(P + Offset, Offset, R);
    UINT32 WireBytes = Le32(P + Offset + 4);
    if (R->Action == CrAdrenoGmuRecordData) {
      ++Plan->DataCount;
      Plan->TotalDataBytes += WireBytes;
    } else if (R->Action == CrAdrenoGmuRecordPreallocation)
      ++Plan->PreallocationCount;
    else if (R->Action == CrAdrenoGmuRecordVersion) {
      UINT32 Index = R->Type - CR_ADRENO_GMU_TYPE_CORE_VERSION;
      Plan->VersionMask |= 1U << Index;
      Plan->Versions[Index] = R->Value;
    }
    Offset = R->PayloadOffset + WireBytes;
  }
  return CR_SUCCESS;
}

CR_STATUS CrAdrenoSqeInspect(CONST VOID *Image, UINTN ImageBytes,
                             CR_ADRENO_SQE_PLAN *Plan) {
  if (!Separate(Image, ImageBytes, Plan, sizeof(*Plan)))
    return CR_INVALID_PARAMETER;
  if (ImageBytes < 8 || ImageBytes > CR_ADRENO_FIRMWARE_MAX_BYTES ||
      (ImageBytes & 3U) != 0) return CR_BAD_BUFFER_SIZE;
  CONST UINT8 *P = (CONST UINT8 *)Image;
  CR_ADRENO_SQE_PLAN Result;
  cr_memset(&Result, 0, sizeof(Result));
  Result.Version = CR_ADRENO_FIRMWARE_PLAN_VERSION;
  Result.PrefixWord = Le32(P);
  Result.CodeOffset = 4;
  Result.CodeBytes = (UINT32)ImageBytes - 4U;
  Result.CodeWords = Result.CodeBytes / 4U;
  Result.RawCodeVersion = Le32(P + 4);
  Result.CodeVersion = Result.RawCodeVersion;
  if ((Result.RawCodeVersion & 0xfU) == 0xaU) {
    if (ImageBytes < 16) return CR_BAD_BUFFER_SIZE;
    Result.CodeVersion = (Result.RawCodeVersion & ~0xfffU) |
                         ((Le32(P + 12) & 0xfff000U) >> 12);
    Result.Flags = CR_ADRENO_SQE_PATCHED_VERSION;
  }
  cr_memcpy(Plan, &Result, sizeof(Result));
  return CR_SUCCESS;
}
