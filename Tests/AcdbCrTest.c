/* Actual reader tests: unchanged public files plus explicitly synthetic cases.
   SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AcdbCrLib.h>
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "AcdbFixtures.h"

static unsigned Checks;
#define CHECK(X) do { ++Checks; assert(X); } while (0)

static void Put32(UINT8 *P, UINT32 Value)
{
  for (unsigned I = 0; I < 4; ++I) {
    P[I] = (UINT8)(Value >> (8U * I));
  }
}

static void AllFail(const void *Data, size_t Bytes, CR_STATUS Expected)
{
  CR_ACDB_INFO Info, SavedInfo;
  CR_ACDB_CHUNK Chunk, SavedChunk;
  CR_ACDB_CODEC Codec, SavedCodec;
  CR_ACDB_SPAN Oem, SavedOem;

  memset(&Info, 0xa7, sizeof(Info)); SavedInfo = Info;
  memset(&Chunk, 0xa7, sizeof(Chunk)); SavedChunk = Chunk;
  memset(&Codec, 0xa7, sizeof(Codec)); SavedCodec = Codec;
  memset(&Oem, 0xa7, sizeof(Oem)); SavedOem = Oem;
  CHECK(CrAcdbInspect(Data, Bytes, &Info) == Expected);
  CHECK(CrAcdbGetChunk(Data, Bytes, 0, &Chunk) == Expected);
  CHECK(CrAcdbFindChunk(Data, Bytes, CR_ACDB_HEAD_ID, &Chunk) == Expected);
  CHECK(CrAcdbGetCodec(Data, Bytes, 0, &Codec) == Expected);
  CHECK(CrAcdbGetOem(Data, Bytes, &Oem) == Expected);
  CHECK(!memcmp(&Info, &SavedInfo, sizeof(Info)));
  CHECK(!memcmp(&Chunk, &SavedChunk, sizeof(Chunk)));
  CHECK(!memcmp(&Codec, &SavedCodec, sizeof(Codec)));
  CHECK(!memcmp(&Oem, &SavedOem, sizeof(Oem)));
}

/* Intentionally synthetic: HEAD with two codec triples and non-NUL OEM. */
static const UINT8 Synthetic[] = {
  0x41,0x43,0x44,0x42, 0,0,0,0, 55,0,0,0,
  'H','E','A','D', 47,0,0,0,
  1,0,0,0, 2,3,4,5, 2,0,0,0,
  0x55,0x38,0,0, 1,0,0,0, 2,0,0,0,
  0x35,0x83,0,0, 3,0,0,0, 4,0,0,0,
  0x12,0x34,0x56,0x78, 3,0,0,0, 'A','B','C',
};

static size_t Make(UINT8 *Data)
{
  /* Use exactly one HEAD with two codecs: 12 + 8 + 47 = 67 bytes. */
  memcpy(Data, Synthetic, 67);
  Put32(Data + 8, 55);
  return 67;
}

static void SyntheticCases(void)
{
  union { UINT64 Align; UINT8 Bytes[256]; } Store, Saved;
  UINT8 *Data = Store.Bytes;
  size_t Bytes = Make(Data);
  CR_ACDB_INFO Info;
  CR_ACDB_CODEC Codec, SavedCodec;
  CR_ACDB_CHUNK Chunk, SavedChunk;
  CR_ACDB_SPAN Oem;

  CHECK(CrAcdbInspect(Data, Bytes, &Info) == CR_SUCCESS);
  CHECK(Info.CodecCount == 2 && Info.ChunkCount == 1);
  CHECK(Info.DateModified == 0x78563412 && Info.FileVersion[0] == 2);
  CHECK(CrAcdbGetCodec(Data, Bytes, 1, &Codec) == CR_SUCCESS);
  CHECK(Codec.Id == 0x8335 && Codec.Major == 3 && Codec.Minor == 4);
  CHECK(CrAcdbGetOem(Data, Bytes, &Oem) == CR_SUCCESS);
  CHECK(Oem.Offset == 64 && Oem.Bytes == 3);
  CHECK(!memcmp(Data + Oem.Offset, "ABC", 3));

  /* Output alias of any backing byte must preserve the entire source. */
  Saved = Store;
  for (unsigned Offset = 0; Offset < Bytes; Offset += 4) {
    CHECK(CrAcdbInspect(Data, Bytes, (CR_ACDB_INFO *)(Data + Offset)) == CR_INVALID_PARAMETER);
    CHECK(CrAcdbGetChunk(Data, Bytes, 0, (CR_ACDB_CHUNK *)(Data + Offset)) == CR_INVALID_PARAMETER);
    CHECK(CrAcdbFindChunk(Data, Bytes, CR_ACDB_HEAD_ID,
                         (CR_ACDB_CHUNK *)(Data + Offset)) == CR_INVALID_PARAMETER);
    CHECK(CrAcdbGetCodec(Data, Bytes, 0, (CR_ACDB_CODEC *)(Data + Offset)) == CR_INVALID_PARAMETER);
    CHECK(CrAcdbGetOem(Data, Bytes, (CR_ACDB_SPAN *)(Data + Offset)) == CR_INVALID_PARAMETER);
    CHECK(!memcmp(&Saved, &Store, sizeof(Store)));
  }
  CHECK(CrAcdbInspect(Data + 4, Bytes - 4, (CR_ACDB_INFO *)Data) == CR_INVALID_PARAMETER);
  CHECK(CrAcdbInspect(Data, Bytes, (CR_ACDB_INFO *)(Data + 129)) == CR_INVALID_PARAMETER);
  CHECK(CrAcdbInspect(Data, Bytes, NULL) == CR_INVALID_PARAMETER);
  AllFail(NULL, Bytes, CR_INVALID_PARAMETER);
  AllFail((void *)(UINTN)(((UINTN)-1) - 1U), 3, CR_INVALID_PARAMETER);
  AllFail(Data, (UINTN)CR_MAX_UINT32 + 1U, CR_BAD_BUFFER_SIZE);
  CHECK(CrAcdbInspect(Data, Bytes, (void *)(UINTN)(((UINTN)-1) - 3U)) == CR_INVALID_PARAMETER);

  /* Size, magic and type checks are independent, including AND-bug cases. */
  Data[0] ^= 1; AllFail(Data, Bytes, CR_COMPROMISED_DATA); Data[0] ^= 1;
  Put32(Data + 4, 1); AllFail(Data, Bytes, CR_UNSUPPORTED); Put32(Data + 4, 0);
  for (size_t Size = 0; Size < Bytes; ++Size) {
    AllFail(Data, Size, CR_COMPROMISED_DATA);
  }
  Put32(Data + 8, UINT32_MAX); AllFail(Data, Bytes, CR_COMPROMISED_DATA);
  Bytes = Make(Data);
  Put32(Data + 16, UINT32_MAX); AllFail(Data, Bytes, CR_COMPROMISED_DATA);
  Bytes = Make(Data);
  Put32(Data + 20, 2); AllFail(Data, Bytes, CR_UNSUPPORTED);
  Bytes = Make(Data);
  Put32(Data + 28, UINT32_MAX); AllFail(Data, Bytes, CR_COMPROMISED_DATA);
  Bytes = Make(Data);
  Put32(Data + 60, UINT32_MAX); AllFail(Data, Bytes, CR_COMPROMISED_DATA);
  Bytes = Make(Data);
  /* Full file framing but every possible truncated HEAD v1 prefix. */
  for (unsigned Size = 0; Size < 47; ++Size) {
    Put32(Data + 8, 8 + Size); Put32(Data + 16, Size);
    AllFail(Data, 20 + Size, CR_COMPROMISED_DATA);
  }
  Bytes = Make(Data);
  /* Duplicate HEAD, including one after the otherwise valid metadata. */
  memcpy(Data + Bytes, Data + 12, 55);
  Put32(Data + 8, (UINT32)Bytes + 55U - 12U);
  AllFail(Data, Bytes + 55U, CR_COMPROMISED_DATA);
  Bytes = Make(Data);
  Put32(Data + 12, 0x41414141); AllFail(Data, Bytes, CR_COMPROMISED_DATA);
  Bytes = Make(Data);

  /* Two repeated unknown zero-byte chunks are iterable, ambiguous by ID. */
  Put32(Data + Bytes, 0x11223344); Put32(Data + Bytes + 4, 0);
  Put32(Data + Bytes + 8, 0x11223344); Put32(Data + Bytes + 12, 0);
  Bytes += 16; Put32(Data + 8, (UINT32)Bytes - 12U);
  CHECK(CrAcdbInspect(Data, Bytes, &Info) == CR_SUCCESS && Info.ChunkCount == 3);
  CHECK(CrAcdbGetChunk(Data, Bytes, 2, &Chunk) == CR_SUCCESS);
  CHECK(Chunk.Bytes == 0 && Chunk.Offset == Bytes);
  SavedChunk = Chunk;
  CHECK(CrAcdbFindChunk(Data, Bytes, 0x11223344, &Chunk) == CR_COMPROMISED_DATA);
  CHECK(!memcmp(&Chunk, &SavedChunk, sizeof(Chunk)));
  CHECK(CrAcdbGetChunk(Data, Bytes, UINT32_MAX, &Chunk) == CR_NOT_FOUND);
  CHECK(CrAcdbFindChunk(Data, Bytes, 0, &Chunk) == CR_NOT_FOUND);
  CHECK(!memcmp(&Chunk, &SavedChunk, sizeof(Chunk)));
  SavedCodec = Codec;
  CHECK(CrAcdbGetCodec(Data, Bytes, UINT32_MAX, &Codec) == CR_NOT_FOUND);
  CHECK(!memcmp(&Codec, &SavedCodec, sizeof(Codec)));
  /* A malformed suffix must reject lookup of a valid earlier HEAD. */
  Put32(Data + Bytes - 4, 1);
  AllFail(Data, Bytes, CR_COMPROMISED_DATA);

  /* HEAD after another unaligned chunk, zero codecs and empty OEM. */
  memset(Data, 0, 256);
  memcpy(Data, "ACDB", 4); Put32(Data + 8, 37);
  memcpy(Data + 12, "ABCD", 4); Put32(Data + 16, 1); Data[20] = 7;
  memcpy(Data + 21, "HEAD", 4); Put32(Data + 25, 20); Put32(Data + 29, 1);
  CHECK(CrAcdbInspect(Data, 49, &Info) == CR_SUCCESS);
  CHECK(Info.Header.Offset == 29 && Info.CodecCount == 0 && Info.Oem.Bytes == 0);
  CHECK(CrAcdbGetCodec(Data, 49, 0, &Codec) == CR_NOT_FOUND);
  CHECK(CrAcdbGetOem(Data, 49, &Oem) == CR_SUCCESS && Oem.Offset == 49);
}

static void RealFile(const char *Path, const FIXTURE *Expected)
{
  FILE *File = fopen(Path, "rb");
  CHECK(File != NULL);
  UINT8 *Storage = malloc(Expected->Bytes + 17U);
  CHECK(Storage != NULL);
  /* Every genuine fixture deliberately starts at an unaligned address. */
  UINT8 *Data = Storage + 1;
  CHECK(fread(Data, 1, Expected->Bytes, File) == Expected->Bytes);
  CHECK(fgetc(File) == EOF);
  CHECK(fclose(File) == 0);
  CR_ACDB_INFO Info;
  CR_ACDB_CHUNK Chunk, ById;
  CR_ACDB_SPAN Oem;
  CHECK(CrAcdbInspect(Data, Expected->Bytes, &Info) == CR_SUCCESS);
  CHECK(!memcmp(&Info, &Expected->Info, sizeof(Info)));
  CHECK(CrAcdbGetOem(Data, Expected->Bytes, &Oem) == CR_SUCCESS);
  CHECK(!memcmp(&Oem, &Expected->Info.Oem, sizeof(Oem)));
  CHECK(!memcmp(Data + Oem.Offset, "CLIVers_1\0", 10));
  for (UINT32 I = 0; I < Info.ChunkCount; ++I) {
    CHECK(CrAcdbGetChunk(Data, Expected->Bytes, I, &Chunk) == CR_SUCCESS);
    CHECK(!memcmp(&Chunk, &Expected->Chunks[I], sizeof(Chunk)));
    CHECK(CrAcdbFindChunk(Data, Expected->Bytes, Chunk.Id, &ById) == CR_SUCCESS);
    CHECK(!memcmp(&Chunk, &ById, sizeof(Chunk)));
    UINT8 Saved[4];
    memcpy(Saved, Data + Chunk.Offset - 4, 4);
    Put32(Data + Chunk.Offset - 4, UINT32_MAX);
    AllFail(Data, Expected->Bytes, CR_COMPROMISED_DATA);
    memcpy(Data + Chunk.Offset - 4, Saved, 4);
  }
  for (UINT32 I = 0; I < Info.CodecCount; ++I) {
    CR_ACDB_CODEC Codec;
    CHECK(CrAcdbGetCodec(Data, Expected->Bytes, I, &Codec) == CR_SUCCESS);
    CHECK(!memcmp(&Codec, &Expected->Codecs[I], sizeof(Codec)));
  }
  /* Every truncation point in both small original fixtures. */
  if (Expected->Bytes < 5000) {
    for (UINT32 I = 0; I < Expected->Bytes; ++I) {
      AllFail(Data, I, CR_COMPROMISED_DATA);
    }
  }
  CHECK(CrAcdbInspect(Data, Expected->Bytes, &Info) == CR_SUCCESS);
  free(Storage);
}

int main(int argc, char **argv)
{
  CHECK(argc == (int)(sizeof(Fixtures) / sizeof(Fixtures[0])) + 1);
  SyntheticCases();
  for (int I = 1; I < argc; ++I) {
    RealFile(argv[I], &Fixtures[I - 1]);
  }
  printf("ACDB actual reader PASS: %u checks, %u unchanged public fixtures\n",
         Checks, (unsigned)(argc - 1));
  return 0;
}
