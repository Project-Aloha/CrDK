/* Actual-source calibration compiler tests against real upstream oracle bytes.
   Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause */
#include <Library/AudioCalCrLib.h>
#include <Library/AudioGprCrLib.h>
#include <Library/AcdbCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned long Checks, Successes, Rejected;
static unsigned Case;
static CONST char *Fixture;
#define CHECK(Test)                                                                                \
  do {                                                                                             \
    ++Checks;                                                                                      \
    if (!(Test)) {                                                                                 \
      fprintf(stderr, "%s case%u line%d: %s\n", Fixture, Case, __LINE__, #Test);                   \
      exit(1);                                                                                     \
    }                                                                                              \
  } while (0)
static UINT32 Word(CONST UINT8 *P)
{
  return (UINT32)P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24);
}
static VOID Put(UINT8 *P, UINT32 Value)
{
  for (unsigned Index = 0; Index < 4; ++Index) {
    P[Index] = (UINT8)(Value >> (Index * 8));
  }
}
static UINT32 ReadWord(FILE *File)
{
  UINT8 Data[4];
  CHECK(fread(Data, 1, 4, File) == 4);
  return Word(Data);
}
static UINT8 *ReadBytes(FILE *File, UINT32 Bytes)
{
  UINT8 *Data = malloc(Bytes ? Bytes : 1);
  CHECK(Data != NULL);
  CHECK(fread(Data, 1, Bytes, File) == Bytes);
  return Data;
}
static BOOLEAN Filled(CONST VOID *Data, UINTN Bytes, UINT8 Value)
{
  CONST UINT8 *P = Data;
  for (UINTN Index = 0; Index < Bytes; ++Index) {
    if (P[Index] != Value) {
      return FALSE;
    }
  }
  return TRUE;
}
static VOID Header(CONST UINT8 *Packet, UINT32 Bytes, UINT32 Token, UINT32 Opcode)
{
  CR_AUDIO_GPR_HEADER Decoded;
  CHECK(CrAudioGprDecode(Packet, Bytes, &Decoded) == CR_SUCCESS);
  CHECK(Decoded.HeaderBytes == 24 && Decoded.PacketBytes == Bytes);
  CHECK(Decoded.SourceDomain == 3 && Decoded.DestinationDomain == 2);
  CHECK(Decoded.SourcePort == 0x100 && Decoded.DestinationPort == 1);
  CHECK(Decoded.Token == Token && Decoded.Opcode == Opcode && Decoded.ClientData == 0);
  CHECK(Word(Packet + 24) == 0 && Word(Packet + 28) == 0 && Word(Packet + 32) == 0);
  CHECK(Word(Packet + 36) == Bytes - 40);
  UINT32 Offset = 40;
  while (Offset < Bytes) {
    CHECK(Bytes - Offset >= 16);
    UINT32 Size = Word(Packet + Offset + 8);
    CHECK(Word(Packet + Offset + 12) == 0);
    CHECK(Size <= Bytes - Offset - 16);
    UINT32 Padding = (8U - (Size & 7U)) & 7U;
    CHECK(Padding <= Bytes - Offset - 16 - Size);
    CHECK(Filled(Packet + Offset + 16 + Size, Padding, 0));
    Offset += 16 + Size + Padding;
  }
  CHECK(Offset == Bytes);
}
static VOID Reject(UINT8 *Data, UINT32 Bytes, CONST CR_AUDIO_CAL_REQUEST *Request, VOID *Workspace,
                   CR_AUDIO_CAL_PLAN *Plan, CR_AUDIO_CAL_RESULT Expected)
{
  CR_AUDIO_CAL_REQUEST Saved = *Request;
  memset(Plan, 0xa5, sizeof(*Plan));
  CR_AUDIO_CAL_RESULT Result =
      CrAudioCalBuildPlan(Data, Bytes, Request, Workspace, CrAudioCalWorkspaceBytes(), Plan);
  CHECK(Result == Expected);
  CHECK(Filled(Plan, sizeof(*Plan), 0xa5));
  CHECK(memcmp(Request, &Saved, sizeof(Saved)) == 0);
}
static VOID Mutation(UINT8 *Data, UINT32 Bytes, CONST CR_AUDIO_CAL_REQUEST *Request,
                     VOID *Workspace, CR_AUDIO_CAL_PLAN *Plan, UINT32 Offset, UINT32 Value,
                     CR_AUDIO_CAL_RESULT Expected)
{
  CHECK(Offset <= Bytes - 4);
  UINT32 Prior = Word(Data + Offset);
  Put(Data + Offset, Value);
  Reject(Data, Bytes, Request, Workspace, Plan, Expected);
  Put(Data + Offset, Prior);
}
static UINT32 Chunk(UINT8 *Data, UINT32 Bytes, CONST char *Name)
{
  CR_ACDB_CHUNK Found;
  CHECK(CrAcdbFindChunk(Data, Bytes, Word((CONST UINT8 *)Name), &Found) == CR_SUCCESS);
  return Found.Offset;
}
static UINT8 *GrowChunk(CONST UINT8 *Data, UINT32 Bytes, UINT32 DataOffset, CONST UINT8 *Append,
                        UINT32 Added)
{
  UINT32 Size = Word(Data + DataOffset - 4), End = DataOffset + Size;
  CHECK(End <= Bytes);
  UINT8 *Result = malloc((size_t)Bytes + Added);
  CHECK(Result != NULL);
  memcpy(Result, Data, End);
  memcpy(Result + End, Append, Added);
  memcpy(Result + End + Added, Data + End, Bytes - End);
  Put(Result + DataOffset - 4, Size + Added);
  Put(Result + 8, Bytes + Added - 12);
  return Result;
}
static VOID DeepMutations(UINT8 *Data, UINT32 Bytes, CONST CR_AUDIO_CAL_REQUEST *Request,
                          VOID *Workspace, CR_AUDIO_CAL_PLAN *Plan)
{
  UINT32 Pool = Chunk(Data, Bytes, "POOL"), Gkvl = Chunk(Data, Bytes, "GKVL");
  UINT32 Cslu = Chunk(Data, Bytes, "CSLU"), Cdlu = Chunk(Data, Bytes, "CDLU");
  UINT32 Cdde = Chunk(Data, Bytes, "CDDE"), Cddo = Chunk(Data, Bytes, "CDDO");
  UINT32 Tmlu = Chunk(Data, Bytes, "TMLU"), Tmde = Chunk(Data, Bytes, "TMDE");
  /* These independent explicit offsets are pinned to the genuine qcm6490
     fixture SHA in inventory.json; assertions detect an accidental mismatch. */
  CONST UINT32 Properties = 85330;
  CHECK(Word(Data + Properties) == 2424 && Word(Data + Properties + 4) == 4);
  CHECK(Word(Data + Properties + 8) == 0xb0000005);
  UINT32 FirstObject = Properties + 8;
  UINT32 Driver = FirstObject + 12;
  UINT32 Spf = Driver + Word(Data + FirstObject + 8) + 4;
  CHECK(Word(Data + Spf) == 1 && Word(Data + Spf + 4) == 0x08001001);
  Mutation(Data, Bytes, Request, Workspace, Plan, Properties, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Properties + 4, 3, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, FirstObject + 4, 0xffffffffU,
           CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, FirstObject + 8, 0xffffffffU,
           CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Driver + 4, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Driver + 16, 5, CrAudioCalUnsupportedDomain);
  Mutation(Data, Bytes, Request, Workspace, Plan, Spf + 8, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Spf + 12, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Spf + 20, 0xffffffffU, CrAudioCalLimit);
  Mutation(Data, Bytes, Request, Workspace, Plan, Spf + 28, 0xffffffffU, CrAudioCalMalformed);
  /* Find exact profile GKVL row independently from its known property span. */
  UINT32 At = Gkvl, End = Gkvl + Word(Data + Gkvl - 4), Row = 0;
  while (At < End) {
    UINT32 Keys = Word(Data + At), Count = Word(Data + At + 4);
    At += 8;
    for (UINT32 Index = 0; Index < Count; ++Index) {
      if (Keys == 4 && Word(Data + At + Keys * 4 + 4) == Properties - Pool) {
        BOOLEAN Same = TRUE;
        for (UINT32 Key = 0; Key < Keys; ++Key) {
          if (Word(Data + At + Key * 4) != Request->GraphKeys[Key].Value) {
            Same = FALSE;
          }
        }
        if (Same) {
          Row = At;
        }
      }
      At += (Keys + 2) * 4;
    }
  }
  CHECK(Row != 0);
  Mutation(Data, Bytes, Request, Workspace, Plan, Row + 16, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Row + 20, 0xffffffffU, CrAudioCalMalformed);
  UINT32 Topology = Pool + Word(Data + Row + 16);
  Mutation(Data, Bytes, Request, Workspace, Plan, Topology + 4, 65, CrAudioCalLimit);
  Mutation(Data, Bytes, Request, Workspace, Plan, Topology + 12, 0xffffffffU, CrAudioCalLimit);
  Mutation(Data, Bytes, Request, Workspace, Plan, Topology + 16, 0xb0000005, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Topology + 24, 0xb0abcdef,
           CrAudioCalUnsupportedExternalGraph);
  /* Default CKV definition/DOT for actual selected SG4. */
  At = Cslu + 4;
  UINT32 Entry = 0;
  for (UINT32 Index = 0; Index < Word(Data + Cslu); ++Index) {
    UINT32 Sg = Word(Data + At), Count = Word(Data + At + 4);
    if (Sg == 0xb0000004) {
      Entry = At + 8;
    }
    At += 8 + Count * 8;
  }
  CHECK(Entry != 0);
  UINT32 Lut = Cdlu + Word(Data + Entry + 4);
  CHECK(Word(Data + Lut) == 0 && Word(Data + Lut + 4) == 1);
  UINT32 Def = Cdde + Word(Data + Lut + 8), Dot = Cddo + Word(Data + Lut + 12);
  CHECK(Word(Data + Def) == Word(Data + Dot) && Word(Data + Def) > 0);
  UINT32 Iid = Word(Data + Def + 4), Pid = Word(Data + Def + 8);
  CHECK(Iid == 0x4012);
  Mutation(Data, Bytes, Request, Workspace, Plan, Entry, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Lut, 1, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Lut + 4, 0, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Def, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Dot, Word(Data + Dot) - 1, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Def + 4, 0xdeadbeef, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Def + 8, 0, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Dot + 4, 0xffffffffU, CrAudioCalMalformed);
  UINT32 Payload = Pool + Word(Data + Dot + 4);
  Mutation(Data, Bytes, Request, Workspace, Plan, Payload, 0xffffffffU, CrAudioCalMalformed);
  /* Valid count+extent persistent table addition, not malformed framing. */
  UINT32 Persist = Chunk(Data, Bytes, "PPTC");
  UINT8 PidBytes[4];
  Put(PidBytes, Pid);
  UINT8 *Changed = GrowChunk(Data, Bytes, Persist, PidBytes, sizeof(PidBytes));
  Put(Changed + Persist, 1);
  Reject(Changed, Bytes + 4, Request, Workspace, Plan, CrAudioCalUnsupportedPersistence);
  free(Changed);
  /* Append a real format offloaded PID table and point GPRO to it. */
  UINT8 Offloaded[12];
  Put(Offloaded, 8);
  Put(Offloaded + 4, 1);
  Put(Offloaded + 8, Pid);
  Changed = GrowChunk(Data, Bytes, Pool, Offloaded, sizeof(Offloaded));
  UINT32 Global = Chunk(Changed, Bytes + 12, "GPRO");
  Put(Changed + Global + 8, Word(Data + Pool - 4));
  Reject(Changed, Bytes + 12, Request, Workspace, Plan, CrAudioCalUnsupportedOffload);
  free(Changed);
  /* Entire command, including GPR+APM headers, has a64KiB policy cap. */
  UINT8 *Huge = calloc(1, 65540);
  CHECK(Huge != NULL);
  Put(Huge, 65536);
  Changed = GrowChunk(Data, Bytes, Pool, Huge, 65540);
  Put(Changed + Dot + 4, Word(Data + Pool - 4));
  Reject(Changed, Bytes + 65540, Request, Workspace, Plan, CrAudioCalLimit);
  free(Huge);
  free(Changed);
  /* A synthetic extension of this genuine fixture exercises a partially
     specified module CKV. No vendor fixture claim is made for the mutation. */
  At = Cslu + 4;
  UINT32 VolumeEntry = 0;
  UINT32 Cakt = Chunk(Data, Bytes, "CAKT");
  for (UINT32 Index = 0; Index < Word(Data + Cslu); ++Index) {
    UINT32 Count = Word(Data + At + 4);
    if (Word(Data + At) == 0xb0000004) {
      for (UINT32 Item = 0; Item < Count; ++Item) {
        UINT32 Current = At + 8 + Item * 8;
        UINT32 Keys = Cakt + Word(Data + Current);
        if (Word(Data + Keys) == 1 && Word(Data + Keys + 4) == 0xa4000000) {
          VolumeEntry = Current;
        }
      }
    }
    At += 8 + Count * 8;
  }
  CHECK(VolumeEntry != 0);
  UINT32 VolumeLut = Cdlu + Word(Data + VolumeEntry + 4);
  CHECK(Word(Data + VolumeLut) == 1 && Word(Data + VolumeLut + 4) == 16);
  Mutation(Data, Bytes, Request, Workspace, Plan, VolumeLut + 8 + 16, Word(Data + VolumeLut + 8),
           CrAudioCalMalformed);
  UINT8 AddedKeys[12];
  Put(AddedKeys, 2);
  Put(AddedKeys + 4, 0xa4000000);
  Put(AddedKeys + 8, 0xa5000000);
  Changed = GrowChunk(Data, Bytes, Cakt, AddedKeys, sizeof(AddedKeys));
  Put(Changed + VolumeEntry, Word(Data + Cakt - 4));
  UINT8 AddedLut[28];
  Put(AddedLut, 2);
  Put(AddedLut + 4, 1);
  Put(AddedLut + 8, 0);
  Put(AddedLut + 12, 48000);
  memcpy(AddedLut + 16, Data + VolumeLut + 12, 12);
  UINT32 MovedCdlu = Chunk(Changed, Bytes + 12, "CDLU");
  UINT8 *Partial = GrowChunk(Changed, Bytes + 12, MovedCdlu, AddedLut, sizeof(AddedLut));
  Put(Partial + VolumeEntry + 4, Word(Changed + MovedCdlu - 4));
  Reject(Partial, Bytes + 40, Request, Workspace, Plan, CrAudioCalMissingCalibrationKey);
  CR_AUDIO_CAL_REQUEST Complete = *Request;
  Complete.CalibrationKeyCount = 2;
  Complete.CalibrationKeys[1] = (CR_AUDIO_CAL_KEY){0xa5000000, 48000};
  CHECK(CrAudioCalBuildPlan(Partial, Bytes + 40, &Complete, Workspace, CrAudioCalWorkspaceBytes(),
                            Plan) == CrAudioCalOk);
  free(Partial);
  free(Changed);
  /* Tag must resolve exactly one declared codec-DMA endpoint. */
  At = Tmlu + 4;
  UINT32 Tag = 0;
  for (UINT32 Index = 0; Index < Word(Data + Tmlu); ++Index, At += 12) {
    if (Word(Data + At) == 0xb0000005 && Word(Data + At + 4) == Request->EndpointTag) {
      Tag = At;
    }
  }
  CHECK(Tag != 0);
  UINT32 Tagged = Tmde + Word(Data + Tag + 8);
  CHECK(Word(Data + Tagged) == 1);
  Mutation(Data, Bytes, Request, Workspace, Plan, Tag + 8, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Tagged, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, Request, Workspace, Plan, Tagged + 8, 0xdeadbeef, CrAudioCalMalformed);
}

static VOID Negatives(UINT8 *Data, UINT32 Bytes, CONST CR_AUDIO_CAL_REQUEST *Original,
                      VOID *Workspace, CR_AUDIO_CAL_PLAN *Plan)
{
  CR_AUDIO_CAL_REQUEST Request = *Original;
  DeepMutations(Data, Bytes, &Request, Workspace, Plan);
  UINT32 Gkvt = Chunk(Data, Bytes, "GKVT"), Gkvl = Chunk(Data, Bytes, "GKVL");
  UINT32 Pool = Chunk(Data, Bytes, "POOL"), Cslu = Chunk(Data, Bytes, "CSLU");
  UINT32 Cakt = Chunk(Data, Bytes, "CAKT"), Cdlu = Chunk(Data, Bytes, "CDLU");
  UINT32 Sclu = Chunk(Data, Bytes, "SCLU"), Sgit = Chunk(Data, Bytes, "SGIT");
  UINT32 Tmlu = Chunk(Data, Bytes, "TMLU"), Gpro = Chunk(Data, Bytes, "GPRO");
  UINT32 Pptc = Chunk(Data, Bytes, "PPTC"), Modm = Chunk(Data, Bytes, "MODM");
  UINT32 Headers[] = {Gkvt, Gkvl, Pool, Cslu, Cakt, Cdlu, Sclu, Sgit, Tmlu, Gpro, Pptc};
  for (UINT32 Index = 0; Index < sizeof(Headers) / sizeof(Headers[0]); ++Index) {
    /* Corrupt every relevant outer chunk extent. */
    Mutation(Data, Bytes, &Request, Workspace, Plan, Headers[Index] - 4, 0xffffffffU,
             CrAudioCalMalformed);
  }
  Mutation(Data, Bytes, &Request, Workspace, Plan, Gkvt, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Gkvt + 4, 33, CrAudioCalLimit);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Gkvt + 8, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Cslu, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Cakt, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Sclu, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Sgit, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Tmlu, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Modm, 1, CrAudioCalUnsupportedAmdb);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Gpro + 8, 0xffffffffU, CrAudioCalMalformed);
  Mutation(Data, Bytes, &Request, Workspace, Plan, Pptc, 1, CrAudioCalMalformed);
  Request.GraphKeys[1] = Request.GraphKeys[0];
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalInvalidArgument);
  Request = *Original;
  Request.GraphKeys[0].Value = 0xdeadbeef;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalNotFound);
  Request = *Original;
  Request.CalibrationKeys[0].Value = 99;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalMissingCalibrationKey);
  Request = *Original;
  Request.CalibrationKeys[0].Key = 0xdeadbeef;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalMissingCalibrationKey);
  Request = *Original;
  Request.EndpointTag = 0xdeadbeef;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalUnsupportedEndpoint);
  Request = *Original;
  Request.OpenToken = Request.CalibrationToken;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalInvalidArgument);
  Request = *Original;
  Request.SampleRate = 44100;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalUnsupportedEndpoint);
  Request = *Original;
  Request.ActiveChannelsMask = 1;
  Reject(Data, Bytes, &Request, Workspace, Plan, CrAudioCalInvalidArgument);
  Request = *Original;
  for (UINT32 Cut = 0; Cut < 64; ++Cut) {
    Reject(Data, Cut, &Request, Workspace, Plan, CrAudioCalMalformed);
    Reject(Data, Bytes - 1 - Cut, &Request, Workspace, Plan, CrAudioCalMalformed);
  }
  memset(Plan, 0xa5, sizeof(*Plan));
  CHECK(CrAudioCalBuildPlan(Data, Bytes, &Request, Workspace, CrAudioCalWorkspaceBytes() - 1,
                            Plan) == CrAudioCalWorkspaceSmall);
  CHECK(Filled(Plan, sizeof(*Plan), 0xa5));
  CHECK(CrAudioCalBuildPlan(Data, Bytes, &Request, (UINT8 *)Workspace + 4,
                            CrAudioCalWorkspaceBytes(), Plan) == CrAudioCalInvalidArgument);
  CHECK(Filled(Plan, sizeof(*Plan), 0xa5));
  CHECK(CrAudioCalBuildPlan(Data, Bytes, &Request, Workspace, CrAudioCalWorkspaceBytes(),
                            (CR_AUDIO_CAL_PLAN *)Data) == CrAudioCalInvalidArgument);
  CHECK(CrAudioCalBuildPlan(Data, Bytes, &Request, Workspace, CrAudioCalWorkspaceBytes(),
                            (CR_AUDIO_CAL_PLAN *)Workspace) == CrAudioCalInvalidArgument);
  CHECK(CrAudioCalBuildPlan(Data, Bytes, (CR_AUDIO_CAL_REQUEST *)Plan, Workspace,
                            CrAudioCalWorkspaceBytes(), Plan) == CrAudioCalInvalidArgument);
  CHECK(CrAudioCalBuildPlan(Data, Bytes, &Request, Data, CrAudioCalWorkspaceBytes(), Plan) ==
        CrAudioCalInvalidArgument);
  CHECK(CrAudioCalBuildPlan((VOID *)((UINTN)-8), 16, &Request, Workspace,
                            CrAudioCalWorkspaceBytes(), Plan) == CrAudioCalInvalidArgument);
  CHECK(Filled(Plan, sizeof(*Plan), 0xa5));
  /* Genuine POOL references may alias. The valid fixture itself contains
     shared payload offsets; successful corpus/oracle checks preserve them. */
  UINT8 *Unaligned = malloc((size_t)Bytes + 1);
  CHECK(Unaligned != NULL);
  memcpy(Unaligned + 1, Data, Bytes);
  CHECK(CrAudioCalBuildPlan(Unaligned + 1, Bytes, &Request, Workspace, CrAudioCalWorkspaceBytes(),
                            Plan) == CrAudioCalOk);
  CHECK(memcmp(Unaligned + 1, Data, Bytes) == 0);
  free(Unaligned);
}
static VOID Corpus(CONST char *Path)
{
  FILE *Vectors = fopen(Path, "rb");
  CHECK(Vectors != NULL);
  CHECK(ReadWord(Vectors) == 0x314c4143U);
  UINT32 PathBytes = ReadWord(Vectors);
  CHECK(PathBytes < 4096);
  char Name[4096];
  CHECK(fread(Name, 1, PathBytes, Vectors) == PathBytes);
  Name[PathBytes] = 0;
  Fixture = Name;
  FILE *Input = fopen(Name, "rb");
  CHECK(Input != NULL);
  CHECK(fseek(Input, 0, SEEK_END) == 0);
  long Length = ftell(Input);
  CHECK(Length > 0 && (unsigned long)Length <= 0xffffffffUL);
  CHECK(fseek(Input, 0, SEEK_SET) == 0);
  UINT32 Bytes = (UINT32)Length;
  UINT8 *Data = ReadBytes(Input, Bytes), *SavedData = malloc(Bytes);
  CHECK(SavedData != NULL);
  memcpy(SavedData, Data, Bytes);
  fclose(Input);
  UINTN WorkBytes = CrAudioCalWorkspaceBytes();
  UINT8 *Workspace = malloc(WorkBytes + 16);
  CHECK(Workspace != NULL);
  CR_AUDIO_CAL_PLAN *Plan = malloc(sizeof(*Plan));
  CHECK(Plan != NULL);
  UINT32 Count = ReadWord(Vectors), Counts[19] = {0};
  for (UINT32 Index = 0; Index < Count; ++Index) {
    Case = ReadWord(Vectors);
    CR_AUDIO_CAL_REQUEST Request, Saved;
    CHECK(fread(&Request, 1, sizeof(Request), Vectors) == sizeof(Request));
    Saved = Request;
    UINT32 Status[6], Sizes[6];
    UINT8 *Expected[6];
    for (UINT32 Part = 0; Part < 6; ++Part) {
      Status[Part] = ReadWord(Vectors);
      Sizes[Part] = ReadWord(Vectors);
      CHECK(Sizes[Part] <= 16U * 1024 * 1024);
      Expected[Part] = ReadBytes(Vectors, Sizes[Part]);
    }
    memset(Plan, 0xa5, sizeof(*Plan));
    memset(Workspace + WorkBytes, 0xe7, 16);
    CR_AUDIO_CAL_RESULT Result =
        CrAudioCalBuildPlan(Data, Bytes, &Request, Workspace, WorkBytes, Plan);
    CHECK(Result <= CrAudioCalUnsupportedExternalGraph);
    ++Counts[Result];
    CHECK(Filled(Workspace + WorkBytes, 16, 0xe7));
    CHECK(memcmp(&Request, &Saved, sizeof(Request)) == 0);
    if (Result == CrAudioCalOk) {
      ++Successes;
      CHECK(Status[0] == 0 && Status[1] == 0 && Status[2] == 0 && Status[4] == 0 && Status[5] == 0);
      CHECK(Status[3] == 0 || (Status[3] == 19 && Sizes[3] == 0));
      CHECK(Plan->OpenPropertyBytes == Sizes[2]);
      CHECK(Plan->OpenBytes == 40 + Sizes[2] + Sizes[3]);
      CHECK(memcmp(Plan->Open + 40, Expected[2], Sizes[2]) == 0);
      CHECK(memcmp(Plan->Open + 40 + Sizes[2], Expected[3], Sizes[3]) == 0);
      CHECK(Plan->CalibrationBytes == Sizes[4] + 40);
      CHECK(memcmp(Plan->Calibration + 40, Expected[4], Sizes[4]) == 0);
      CHECK(Plan->DriverBytes == Sizes[1] && memcmp(Plan->Driver, Expected[1], Sizes[1]) == 0);
      CHECK(Sizes[5] == 8 && Plan->EndpointModule == Word(Expected[5]) &&
            Plan->EndpointInstance == Word(Expected[5] + 4));
      UINT32 Offset = 0, SgCount = 0;
      while (Offset < Sizes[0]) {
        CHECK(Sizes[0] - Offset >= 8 && SgCount < Plan->SubgraphCount);
        CHECK(Word(Expected[0] + Offset) == Plan->Subgraphs[SgCount++].Id);
        Offset += 8 + Word(Expected[0] + Offset + 4) * 4;
      }
      CHECK(Offset == Sizes[0] && SgCount == Plan->SubgraphCount);
      Header(Plan->Open, Plan->OpenBytes, 1, 0x01001000);
      Header(Plan->Calibration, Plan->CalibrationBytes, 2, 0x01001006);
      Header(Plan->Endpoint, Plan->EndpointBytes, 3, 0x01001006);
      CHECK(Plan->EndpointBytes == 104);
      CHECK(Word(Plan->Endpoint + 40) == Plan->EndpointInstance);
      CHECK(Word(Plan->Endpoint + 44) == 0x08001063 && Word(Plan->Endpoint + 48) == 12);
      CHECK(Word(Plan->Endpoint + 56) == 1 && Word(Plan->Endpoint + 60) == 0 &&
            Word(Plan->Endpoint + 64) == 3);
      CHECK(Word(Plan->Endpoint + 72) == Plan->EndpointInstance);
      CHECK(Word(Plan->Endpoint + 76) == 0x08001017 && Word(Plan->Endpoint + 80) == 12);
      CHECK(Word(Plan->Endpoint + 88) == 48000 && Word(Plan->Endpoint + 92) == 0x00020010 &&
            Word(Plan->Endpoint + 96) == 1);
      if (strstr(Name, "/qcm6490/") != NULL && Index == Count - 16) {
        Negatives(Data, Bytes, &Request, Workspace, Plan);
      }
    } else {
      ++Rejected;
      CHECK(Result != CrAudioCalMalformed && Result != CrAudioCalNotFound &&
            Result != CrAudioCalWorkspaceSmall);
      CHECK(Result != CrAudioCalInvalidArgument || Request.GraphKeyCount == 0);
      CHECK(Filled(Plan, sizeof(*Plan), 0xa5));
    }
    for (UINT32 Part = 0; Part < 6; ++Part) {
      free(Expected[Part]);
    }
  }
  CHECK(fgetc(Vectors) == EOF);
  fclose(Vectors);
  CHECK(memcmp(Data, SavedData, Bytes) == 0);
  printf("%s: ", Name);
  for (UINT32 Index = 0; Index < 19; ++Index) {
    if (Counts[Index]) {
      printf("result%u=%u ", Index, Counts[Index]);
    }
  }
  printf("\n");
  free(Data);
  free(SavedData);
  free(Plan);
  free(Workspace);
}
int main(int Argc, char **Argv)
{
  Fixture = "startup";
  CHECK(sizeof(CR_AUDIO_CAL_REQUEST) == 564);
  CHECK(sizeof(CR_AUDIO_CAL_PLAN) == 153288);
  CHECK(Argc > 1);
  for (int Index = 1; Index < Argc; ++Index) {
    Corpus(Argv[Index]);
  }
  CHECK(Successes > 0 && Rejected > 0);
  printf("AudioCal: %lu checks PASS; %lu complete plans byte-exact to source oracle, %lu explicit "
         "unsupported/empty requests\n",
         Checks, Successes, Rejected);
  return 0;
}
