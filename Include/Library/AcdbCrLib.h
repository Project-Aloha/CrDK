/** Bounded AudioReach ACDB container inspection.
    Copyright (c) 2026 Project Aloha Authors.
    SPDX-License-Identifier: BSD-3-Clause */
#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define CR_ACDB_HEAD_ID 0x44414548U

typedef struct {
  UINT32 Offset;
  UINT32 Bytes;
} CR_ACDB_SPAN;

typedef struct {
  UINT32 Id;
  UINT32 Offset;
  UINT32 Bytes;
} CR_ACDB_CHUNK;

typedef struct {
  UINT32 Id;
  UINT32 Major;
  UINT32 Minor;
} CR_ACDB_CODEC;

typedef struct {
  UINT32 HeaderVersion;
  UINT8 FileVersion[4]; /* major, minor, revision, CPL information */
  UINT32 CodecCount;
  UINT32 DateModified;
  UINT32 ChunkCount;
  CR_ACDB_SPAN Header;
  CR_ACDB_SPAN Oem;
} CR_ACDB_INFO;

CR_STATIC_ASSERT(sizeof(CR_ACDB_SPAN) == 8, "ACDB span layout");
CR_STATIC_ASSERT(sizeof(CR_ACDB_CHUNK) == 12, "ACDB chunk layout");
CR_STATIC_ASSERT(sizeof(CR_ACDB_CODEC) == 12, "ACDB codec layout");
CR_STATIC_ASSERT(sizeof(CR_ACDB_INFO) == 36, "ACDB info layout");

/* All APIs require readable, caller-owned Data[Bytes] immutable throughout
   each call. Data may be unaligned; Bytes is limited to UINT32_MAX. Output
   must be naturally DWORD aligned, writable, and disjoint from the entire
   backing buffer. Each call validates framing and HEAD afresh. No retained
   pointers, allocation, IO or runtime state. Every failure preserves output.

   Returned spans are offsets into the original buffer, including zero-length
   chunks/OEM. They remain usable only while those same immutable bytes are
   alive; a copied INFO is not a validated handle for another buffer.

   Only type 0 containers with exactly one HEAD version 1 are supported.
   File-version bytes are metadata, not an assumed DSP compatibility claim.
   Unknown chunks and bounded trailing HEAD bytes are preserved. OEM is a
   byte span, not necessarily a NUL-terminated string. Nested graph/calibration
   table contents are NOT validated or interpreted by this library yet.

   INVALID_PARAMETER: pointer/range/alignment/alias contract violation.
   BAD_BUFFER_SIZE: Bytes exceeds UINT32_MAX.
   COMPROMISED_DATA: malformed framing/HEAD or missing/duplicate HEAD.
   UNSUPPORTED: file type or HEAD version is unsupported.
   NOT_FOUND: valid file but requested index/ID does not exist.
   FindChunk rejects duplicate matching IDs with COMPROMISED_DATA; inspection
   and indexed iteration allow repeated unknown chunks without choosing one. */
CR_STATUS CrAcdbInspect(CONST VOID *Data, UINTN Bytes, CR_ACDB_INFO *Info);
CR_STATUS CrAcdbGetChunk(CONST VOID *Data, UINTN Bytes, UINT32 Index,
                        CR_ACDB_CHUNK *Chunk);
CR_STATUS CrAcdbFindChunk(CONST VOID *Data, UINTN Bytes, UINT32 Id,
                         CR_ACDB_CHUNK *Chunk);
CR_STATUS CrAcdbGetCodec(CONST VOID *Data, UINTN Bytes, UINT32 Index,
                        CR_ACDB_CODEC *Codec);
CR_STATUS CrAcdbGetOem(CONST VOID *Data, UINTN Bytes, CR_ACDB_SPAN *Oem);
