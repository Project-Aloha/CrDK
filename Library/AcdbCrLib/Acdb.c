/* Copyright (c) 2026 Project Aloha Authors.
   SPDX-License-Identifier: BSD-3-Clause

   Original bounded reader of the format documented by Qualcomm AudioReach
   acdb_parser.c/.h at 3094583c726710b66eb0204fc5d6a7b336d9e575.
   See README.md and LICENSE for provenance and scope. */
#include <Library/AcdbCrLib.h>

#define ACDB_MAGIC 0x42444341U
#define MAX_N ((UINTN)-1)

static UINT32 Read32(CONST UINT8 *Data)
{
  return (UINT32)Data[0] | ((UINT32)Data[1] << 8) |
         ((UINT32)Data[2] << 16) | ((UINT32)Data[3] << 24);
}

static CR_STATUS Arguments(CONST VOID *Data, UINTN Bytes,
                           CONST VOID *Output, UINTN OutputBytes)
{
  UINTN InputAddress = (UINTN)Data;
  UINTN OutputAddress = (UINTN)Output;

  if (Data == NULL || Output == NULL || (OutputAddress & 3U) != 0 ||
      InputAddress > MAX_N - Bytes || OutputAddress > MAX_N - OutputBytes) {
    return CR_INVALID_PARAMETER;
  }
  if (Bytes > CR_MAX_UINT32) {
    return CR_BAD_BUFFER_SIZE;
  }
  if (InputAddress < OutputAddress + OutputBytes &&
      OutputAddress < InputAddress + Bytes) {
    return CR_INVALID_PARAMETER;
  }
  return CR_SUCCESS;
}

/* Call only after pointer preflight. Output is local until the public API's
   sole successful publication. Every addition is preceded by a subtraction
   bound, so hostile UINT32 counts cannot wrap even on 32-bit builds. */
static CR_STATUS Parse(CONST UINT8 *Data, UINT32 Bytes, CR_ACDB_INFO *Info)
{
  CR_ACDB_INFO Result = {0};
  UINT32 Offset = 12;
  BOOLEAN HaveHeader = FALSE;

  if (Bytes < 12 || Read32(Data) != ACDB_MAGIC) {
    return CR_COMPROMISED_DATA;
  }
  if (Read32(Data + 4) != 0) {
    return CR_UNSUPPORTED;
  }
  if (Read32(Data + 8) != Bytes - 12U) {
    return CR_COMPROMISED_DATA;
  }
  while (Offset < Bytes) {
    UINT32 Id;
    UINT32 Size;

    if (Bytes - Offset < 8) {
      return CR_COMPROMISED_DATA;
    }
    Id = Read32(Data + Offset);
    Size = Read32(Data + Offset + 4U);
    Offset += 8;
    if (Size > Bytes - Offset) {
      return CR_COMPROMISED_DATA;
    }
    if (Id == CR_ACDB_HEAD_ID) {
      if (HaveHeader) {
        return CR_COMPROMISED_DATA;
      }
      HaveHeader = TRUE;
      Result.Header.Offset = Offset;
      Result.Header.Bytes = Size;
    }
    ++Result.ChunkCount; /* At most (Bytes - 12) / 8. */
    Offset += Size;
  }
  if (!HaveHeader || Result.Header.Bytes < 4) {
    return CR_COMPROMISED_DATA;
  }

  CONST UINT8 *Header = Data + Result.Header.Offset;
  Result.HeaderVersion = Read32(Header);
  if (Result.HeaderVersion != 1) {
    return CR_UNSUPPORTED;
  }
  /* version + version bytes + count + timestamp + OEM length */
  if (Result.Header.Bytes < 20) {
    return CR_COMPROMISED_DATA;
  }
  for (UINT32 Index = 0; Index < 4; ++Index) {
    Result.FileVersion[Index] = Header[4U + Index];
  }
  Result.CodecCount = Read32(Header + 8);
  if (Result.CodecCount > (Result.Header.Bytes - 20U) / 12U) {
    return CR_COMPROMISED_DATA;
  }
  Offset = 12U + Result.CodecCount * 12U;
  Result.DateModified = Read32(Header + Offset);
  Result.Oem.Bytes = Read32(Header + Offset + 4U);
  Offset += 8;
  if (Result.Oem.Bytes > Result.Header.Bytes - Offset) {
    return CR_COMPROMISED_DATA;
  }
  Result.Oem.Offset = Result.Header.Offset + Offset;
  *Info = Result;
  return CR_SUCCESS;
}

CR_STATUS CrAcdbInspect(CONST VOID *Data, UINTN Bytes, CR_ACDB_INFO *Info)
{
  CR_ACDB_INFO Result;
  CR_STATUS Status = Arguments(Data, Bytes, Info, sizeof(*Info));

  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = Parse(Data, (UINT32)Bytes, &Result);
  if (!CR_ERROR(Status)) {
    *Info = Result;
  }
  return Status;
}

static CR_STATUS SelectChunk(CONST VOID *Data, UINTN Bytes, UINT32 Selector,
                            BOOLEAN ById, CR_ACDB_CHUNK *Chunk)
{
  CR_ACDB_INFO Info;
  CR_ACDB_CHUNK Result = {0};
  CONST UINT8 *Buffer = Data;
  BOOLEAN Found = FALSE;
  UINT32 Offset = 12;
  CR_STATUS Status = Arguments(Data, Bytes, Chunk, sizeof(*Chunk));

  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = Parse(Data, (UINT32)Bytes, &Info);
  if (CR_ERROR(Status)) {
    return Status;
  }
  for (UINT32 Index = 0; Index < Info.ChunkCount; ++Index) {
    UINT32 Id = Read32(Buffer + Offset);
    UINT32 Size = Read32(Buffer + Offset + 4U);

    Offset += 8;
    if ((ById && Id == Selector) || (!ById && Index == Selector)) {
      if (Found) {
        return CR_COMPROMISED_DATA;
      }
      Found = TRUE;
      Result.Id = Id;
      Result.Offset = Offset;
      Result.Bytes = Size;
    }
    Offset += Size;
  }
  if (!Found) {
    return CR_NOT_FOUND;
  }
  *Chunk = Result;
  return CR_SUCCESS;
}

CR_STATUS CrAcdbGetChunk(CONST VOID *Data, UINTN Bytes, UINT32 Index,
                        CR_ACDB_CHUNK *Chunk)
{
  return SelectChunk(Data, Bytes, Index, FALSE, Chunk);
}

CR_STATUS CrAcdbFindChunk(CONST VOID *Data, UINTN Bytes, UINT32 Id,
                         CR_ACDB_CHUNK *Chunk)
{
  return SelectChunk(Data, Bytes, Id, TRUE, Chunk);
}

CR_STATUS CrAcdbGetCodec(CONST VOID *Data, UINTN Bytes, UINT32 Index,
                        CR_ACDB_CODEC *Codec)
{
  CR_ACDB_INFO Info;
  CR_ACDB_CODEC Result;
  CR_STATUS Status = Arguments(Data, Bytes, Codec, sizeof(*Codec));

  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = Parse(Data, (UINT32)Bytes, &Info);
  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Index >= Info.CodecCount) {
    return CR_NOT_FOUND;
  }
  CONST UINT8 *Entry = (CONST UINT8 *)Data + Info.Header.Offset + 12U + Index * 12U;
  Result.Id = Read32(Entry);
  Result.Major = Read32(Entry + 4);
  Result.Minor = Read32(Entry + 8);
  *Codec = Result;
  return CR_SUCCESS;
}

CR_STATUS CrAcdbGetOem(CONST VOID *Data, UINTN Bytes, CR_ACDB_SPAN *Oem)
{
  CR_ACDB_INFO Info;
  CR_STATUS Status = Arguments(Data, Bytes, Oem, sizeof(*Oem));

  if (CR_ERROR(Status)) {
    return Status;
  }
  Status = Parse(Data, (UINT32)Bytes, &Info);
  if (!CR_ERROR(Status)) {
    *Oem = Info.Oem;
  }
  return Status;
}
