/* Copyright (c) 2026 Project Aloha Authors. SPDX-License-Identifier: BSD-3-Clause
   Original bounded implementation of pinned Qualcomm AudioReach file/wire
   facts. See README.md and LICENSE for provenance and qualifications. */
#include <Library/AcdbCrLib.h>
#include <Library/AudioCalCrLib.h>

#define FOURCC(A, B, C, D)                                                                         \
  ((UINT32)(A) | ((UINT32)(B) << 8) | ((UINT32)(C) << 16) | ((UINT32)(D) << 24))
#define MAX_N ((UINTN)-1)
#define TRY(Expression)                                                                            \
  do {                                                                                             \
    CR_AUDIO_CAL_RESULT Result_ = (Expression);                                                    \
    if (Result_ != CrAudioCalOk) {                                                                 \
      return Result_;                                                                              \
    }                                                                                              \
  } while (0)

typedef struct {
  CONST UINT8 *Data;
  UINT32 Bytes;
} VIEW;
typedef struct {
  VIEW View;
  UINT32 Position;
} CURSOR;
typedef struct {
  UINT32 Instance;
  UINT32 Count;
} REFERENCE;
enum {
  Gkvt,
  Gkvl,
  Pool,
  Cslu,
  Cakt,
  Cdlu,
  Cdde,
  Cddo,
  Sclu,
  Scde,
  Scdo,
  Tmlu,
  Tmde,
  Pptc,
  Sgit,
  Gpro,
  Modm,
  Btup,
  Gclu,
  Gppm,
  Hasi,
  ChunkCount
};
static CONST UINT32 ChunkIds[ChunkCount] = {
    FOURCC('G', 'K', 'V', 'T'), FOURCC('G', 'K', 'V', 'L'), FOURCC('P', 'O', 'O', 'L'),
    FOURCC('C', 'S', 'L', 'U'), FOURCC('C', 'A', 'K', 'T'), FOURCC('C', 'D', 'L', 'U'),
    FOURCC('C', 'D', 'D', 'E'), FOURCC('C', 'D', 'D', 'O'), FOURCC('S', 'C', 'L', 'U'),
    FOURCC('S', 'C', 'D', 'E'), FOURCC('S', 'C', 'D', 'O'), FOURCC('T', 'M', 'L', 'U'),
    FOURCC('T', 'M', 'D', 'E'), FOURCC('P', 'P', 'T', 'C'), FOURCC('S', 'G', 'I', 'T'),
    FOURCC('G', 'P', 'R', 'O'), FOURCC('M', 'O', 'D', 'M'), FOURCC('B', 'T', 'U', 'P'),
    FOURCC('G', 'C', 'L', 'U'), FOURCC('M', 'P', 'P', 'G'), FOURCC('H', 'A', 'S', 'I')};
typedef struct {
  UINT32 Source, SourcePort, Destination, DestinationPort;
} DATA_LINK;
typedef struct {
  CR_AUDIO_CAL_PLAN Plan;
  CR_AUDIO_CAL_PLAYBACK_ENDPOINTS Playback;
  DATA_LINK DataLinks[CR_AUDIO_CAL_MAX_EDGES];
  UINT32 DataLinkCount;
  VIEW Chunks[ChunkCount];
  VIEW Offloaded;
  REFERENCE References[CR_AUDIO_CAL_MAX_PARAMETERS];
  UINT32 ReferenceCount;
  UINT32 Declared[CR_AUDIO_CAL_MAX_MODULES];
  UINT32 UsedKeys;
} WORK;

static UINT32 Read32(CONST UINT8 *Data)
{
  return (UINT32)Data[0] | ((UINT32)Data[1] << 8) | ((UINT32)Data[2] << 16) |
         ((UINT32)Data[3] << 24);
}
static VOID Write32(UINT8 *Data, UINT32 Value)
{
  for (UINT32 Index = 0; Index < 4; ++Index) {
    Data[Index] = (UINT8)(Value >> (8U * Index));
  }
}
static VOID Copy(VOID *To, CONST VOID *From, UINTN Bytes)
{
  UINT8 *Target = To;
  CONST UINT8 *Source = From;
  for (UINTN Index = 0; Index < Bytes; ++Index) {
    Target[Index] = Source[Index];
  }
}
static VOID Zero(VOID *Data, UINTN Bytes)
{
  UINT8 *Target = Data;
  for (UINTN Index = 0; Index < Bytes; ++Index) {
    Target[Index] = 0;
  }
}
static BOOLEAN Range(CONST VOID *Data, UINTN Bytes)
{
  return Data != NULL && (UINTN)Data <= MAX_N - Bytes;
}
static BOOLEAN Overlap(CONST VOID *A, UINTN ABytes, CONST VOID *B, UINTN BBytes)
{
  return (UINTN)A < (UINTN)B + BBytes && (UINTN)B < (UINTN)A + ABytes;
}
static CR_AUDIO_CAL_RESULT Slice(VIEW Source, UINT32 Offset, UINT32 Bytes, VIEW *Out)
{
  if (Offset > Source.Bytes || Bytes > Source.Bytes - Offset) {
    return CrAudioCalMalformed;
  }
  Out->Data = Source.Data + Offset;
  Out->Bytes = Bytes;
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Take(CURSOR *Cursor, UINT32 Bytes, VIEW *Out)
{
  TRY(Slice(Cursor->View, Cursor->Position, Bytes, Out));
  Cursor->Position += Bytes;
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Word(CURSOR *Cursor, UINT32 *Value)
{
  VIEW Part;
  TRY(Take(Cursor, 4, &Part));
  *Value = Read32(Part.Data);
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Array(CURSOR *Cursor, UINT32 Count, UINT32 Stride, VIEW *Out)
{
  if (Count > (Cursor->View.Bytes - Cursor->Position) / Stride) {
    return CrAudioCalMalformed;
  }
  return Take(Cursor, Count * Stride, Out);
}
static CR_AUDIO_CAL_RESULT PoolObject(WORK *Work, UINT32 Offset, VIEW *Object)
{
  CURSOR Cursor = {Work->Chunks[Pool], Offset};
  UINT32 Bytes;
  if (Offset > Cursor.View.Bytes) {
    return CrAudioCalMalformed;
  }
  TRY(Word(&Cursor, &Bytes));
  return Take(&Cursor, Bytes, Object);
}
static CR_AUDIO_CAL_RESULT Counted(VIEW Source, UINT32 Offset, UINT32 Stride, VIEW *Entries)
{
  CURSOR Cursor = {Source, Offset};
  UINT32 Count;
  if (Offset > Source.Bytes) {
    return CrAudioCalMalformed;
  }
  TRY(Word(&Cursor, &Count));
  return Array(&Cursor, Count, Stride, Entries);
}
static INT32 Compare(CONST UINT8 *A, CONST UINT8 *B, UINT32 Words)
{
  for (UINT32 Index = 0; Index < Words; ++Index) {
    UINT32 Left = Read32(A + Index * 4), Right = Read32(B + Index * 4);
    if (Left != Right) {
      return Left < Right ? -1 : 1;
    }
  }
  return 0;
}
/* Search rows only after validating strictly ascending, unambiguous keys. */
static CR_AUDIO_CAL_RESULT Rows(VIEW Table, UINT32 KeyWords, UINT32 TailWords, CONST UINT8 *Keys,
                                VIEW *Match)
{
  UINT32 Stride;
  BOOLEAN Found = FALSE;
  if (KeyWords > CR_AUDIO_CAL_MAX_KEYS || TailWords == 0 || TailWords > 3) {
    return CrAudioCalLimit;
  }
  Stride = (KeyWords + TailWords) * 4;
  if (Table.Bytes % Stride != 0) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Offset = 0; Offset < Table.Bytes; Offset += Stride) {
    CONST UINT8 *Row = Table.Data + Offset;
    if (Offset != 0 && Compare(Row - Stride, Row, KeyWords) >= 0) {
      return CrAudioCalMalformed;
    }
    if (Compare(Row, Keys, KeyWords) == 0) {
      Match->Data = Row + KeyWords * 4;
      Match->Bytes = TailWords * 4;
      Found = TRUE;
    }
  }
  return Found ? CrAudioCalOk : CrAudioCalNotFound;
}
static CR_AUDIO_CAL_RESULT SortKeys(CR_AUDIO_CAL_KEY *Keys, UINT32 Count)
{
  for (UINT32 Index = 1; Index < Count; ++Index) {
    CR_AUDIO_CAL_KEY Key = Keys[Index];
    UINT32 Position = Index;
    while (Position != 0 && Keys[Position - 1].Key > Key.Key) {
      Keys[Position] = Keys[Position - 1];
      --Position;
    }
    Keys[Position] = Key;
  }
  for (UINT32 Index = 1; Index < Count; ++Index) {
    if (Keys[Index - 1].Key == Keys[Index].Key) {
      return CrAudioCalInvalidArgument;
    }
  }
  return CrAudioCalOk;
}
static INT32 SubgraphIndex(WORK *Work, UINT32 Id)
{
  for (UINT32 Index = 0; Index < Work->Plan.SubgraphCount; ++Index) {
    if (Work->Plan.Subgraphs[Index].Id == Id) {
      return (INT32)Index;
    }
  }
  return -1;
}
static INT32 ModuleIndex(WORK *Work, UINT32 Instance)
{
  for (UINT32 Index = 0; Index < Work->Plan.ModuleCount; ++Index) {
    if (Work->Plan.Modules[Index].Instance == Instance) {
      return (INT32)Index;
    }
  }
  return -1;
}
static CR_AUDIO_CAL_RESULT Chunks(WORK *Work, CONST VOID *Data, UINTN Bytes)
{
  CR_ACDB_INFO Info;
  CR_STATUS Status = CrAcdbInspect(Data, Bytes, &Info);
  if (Status == CR_UNSUPPORTED) {
    return CrAudioCalUnsupportedContainer;
  }
  if (CR_ERROR(Status)) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < ChunkCount; ++Index) {
    CR_ACDB_CHUNK Chunk;
    Status = CrAcdbFindChunk(Data, Bytes, ChunkIds[Index], &Chunk);
    if (Status == CR_NOT_FOUND) {
      continue;
    }
    if (CR_ERROR(Status)) {
      return CrAudioCalMalformed;
    }
    Work->Chunks[Index].Data = (CONST UINT8 *)Data + Chunk.Offset;
    Work->Chunks[Index].Bytes = Chunk.Bytes;
  }
  for (UINT32 Index = Gkvt; Index <= Sgit; ++Index) {
    if (Work->Chunks[Index].Data == NULL) {
      return Index == Sgit ? CrAudioCalMissingModuleMap : CrAudioCalMalformed;
    }
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Graph(WORK *Work, UINT32 *PropertyOffset)
{
  CURSOR Cursor = {Work->Chunks[Gkvt], 0};
  UINT32 Tables, PriorKeys = 0, SelectedOffset = 0;
  BOOLEAN Found = FALSE;
  UINT8 Keys[CR_AUDIO_CAL_MAX_KEYS * 4] = {0};
  VIEW Table, Match, Object;
  CR_AUDIO_CAL_REQUEST *Request = &Work->Plan.Request;
  for (UINT32 Index = 0; Index < Request->GraphKeyCount; ++Index) {
    Write32(Keys + Index * 4, Request->GraphKeys[Index].Key);
  }
  TRY(Word(&Cursor, &Tables));
  if (Tables > (Cursor.View.Bytes - Cursor.Position) / 8) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < Tables; ++Index) {
    UINT32 Count, Entries;
    TRY(Word(&Cursor, &Count));
    TRY(Word(&Cursor, &Entries));
    if (Count > CR_AUDIO_CAL_MAX_KEYS) {
      return CrAudioCalLimit;
    }
    if (Index != 0 && Count <= PriorKeys) {
      return CrAudioCalMalformed;
    }
    PriorKeys = Count;
    TRY(Array(&Cursor, Entries, (Count + 1) * 4, &Table));
    /* Every key row itself must be a strictly sorted list of IDs. */
    for (UINT32 Row = 0; Row < Entries; ++Row) {
      for (UINT32 Key = 1; Key < Count; ++Key) {
        CONST UINT8 *At = Table.Data + Row * (Count + 1) * 4 + Key * 4;
        if (Read32(At - 4) >= Read32(At)) {
          return CrAudioCalMalformed;
        }
      }
    }
    CR_AUDIO_CAL_RESULT Result = Rows(Table, Count, 1, Keys, &Match);
    if (Result != CrAudioCalOk && Result != CrAudioCalNotFound) {
      return Result;
    }
    if (Count == Request->GraphKeyCount && Result == CrAudioCalOk) {
      SelectedOffset = Read32(Match.Data);
      Found = TRUE;
    }
  }
  if (Cursor.Position != Cursor.View.Bytes) {
    return CrAudioCalMalformed;
  }
  if (!Found) {
    return CrAudioCalNotFound;
  }
  Cursor.View = Work->Chunks[Gkvl];
  Cursor.Position = SelectedOffset;
  if (Cursor.Position > Cursor.View.Bytes) {
    return CrAudioCalMalformed;
  }
  UINT32 Count, Entries;
  TRY(Word(&Cursor, &Count));
  TRY(Word(&Cursor, &Entries));
  if (Count != Request->GraphKeyCount) {
    return CrAudioCalMalformed;
  }
  TRY(Array(&Cursor, Entries, (Count + 2) * 4, &Table));
  for (UINT32 Index = 0; Index < Count; ++Index) {
    Write32(Keys + Index * 4, Request->GraphKeys[Index].Value);
  }
  TRY(Rows(Table, Count, 2, Keys, &Match));
  *PropertyOffset = Read32(Match.Data + 4);
  TRY(PoolObject(Work, Read32(Match.Data), &Object));
  Cursor.View = Object;
  Cursor.Position = 0;
  TRY(Word(&Cursor, &Count));
  if (Count == 0 || Count > CR_AUDIO_CAL_MAX_SUBGRAPHS) {
    return CrAudioCalLimit;
  }
  for (UINT32 Index = 0; Index < Count; ++Index) {
    UINT32 Id, Destinations;
    TRY(Word(&Cursor, &Id));
    TRY(Word(&Cursor, &Destinations));
    if (Id == 0 || SubgraphIndex(Work, Id) >= 0) {
      return CrAudioCalMalformed;
    }
    Work->Plan.Subgraphs[Work->Plan.SubgraphCount++].Id = Id;
    if (Destinations > CR_AUDIO_CAL_MAX_EDGES - Work->Plan.EdgeCount) {
      return CrAudioCalLimit;
    }
    TRY(Array(&Cursor, Destinations, 4, &Table));
    for (UINT32 Edge = 0; Edge < Destinations; ++Edge) {
      UINT32 Destination = Read32(Table.Data + Edge * 4);
      if (Id == Destination) {
        return CrAudioCalMalformed;
      }
      for (UINT32 Prior = 0; Prior < Work->Plan.EdgeCount; ++Prior) {
        if (Work->Plan.Edges[Prior].Source == Id &&
            Work->Plan.Edges[Prior].Destination == Destination) {
          return CrAudioCalMalformed;
        }
      }
      Work->Plan.Edges[Work->Plan.EdgeCount].Source = Id;
      Work->Plan.Edges[Work->Plan.EdgeCount++].Destination = Destination;
    }
  }
  if (Cursor.Position != Cursor.View.Bytes) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < Work->Plan.EdgeCount; ++Index) {
    if (SubgraphIndex(Work, Work->Plan.Edges[Index].Destination) < 0) {
      return CrAudioCalUnsupportedExternalGraph;
    }
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT ModuleMap(WORK *Work)
{
  CURSOR Cursor = {Work->Chunks[Sgit], 0};
  UINT32 Count, PriorId = 0, Found = 0;
  TRY(Word(&Cursor, &Count));
  if (Count > (Cursor.View.Bytes - Cursor.Position) / 12) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < Count; ++Index) {
    UINT32 Id, Processors, Bytes;
    VIEW Object;
    TRY(Word(&Cursor, &Id));
    TRY(Word(&Cursor, &Processors));
    TRY(Word(&Cursor, &Bytes));
    TRY(Take(&Cursor, Bytes, &Object));
    if (Index != 0 && Id <= PriorId) {
      return CrAudioCalMalformed;
    }
    PriorId = Id;
    if (SubgraphIndex(Work, Id) < 0) {
      continue;
    }
    ++Found;
    CURSOR Modules = {Object, 0};
    if (Processors == 0 || Processors > Object.Bytes / 8) {
      return CrAudioCalMalformed;
    }
    for (UINT32 Processor = 0; Processor < Processors; ++Processor) {
      UINT32 Domain, ModuleCount;
      VIEW List;
      TRY(Word(&Modules, &Domain));
      TRY(Word(&Modules, &ModuleCount));
      TRY(Array(&Modules, ModuleCount, 8, &List));
      if (Domain != 2) {
        return CrAudioCalUnsupportedDomain;
      }
      if (ModuleCount > CR_AUDIO_CAL_MAX_MODULES - Work->Plan.ModuleCount) {
        return CrAudioCalLimit;
      }
      for (UINT32 Module = 0; Module < ModuleCount; ++Module) {
        UINT32 Mid = Read32(List.Data + Module * 8);
        UINT32 Iid = Read32(List.Data + Module * 8 + 4);
        if (Mid == 0 || Iid == 0 || Iid == 1 || ModuleIndex(Work, Iid) >= 0) {
          return CrAudioCalMalformed;
        }
        CR_AUDIO_CAL_MODULE *Output = &Work->Plan.Modules[Work->Plan.ModuleCount++];
        Output->Subgraph = Id;
        Output->Module = Mid;
        Output->Instance = Iid;
      }
    }
    if (Modules.Position != Object.Bytes) {
      return CrAudioCalMalformed;
    }
  }
  if (Cursor.Position != Cursor.View.Bytes) {
    return CrAudioCalMalformed;
  }
  return Found == Work->Plan.SubgraphCount ? CrAudioCalOk : CrAudioCalMissingModuleMap;
}
static CR_AUDIO_CAL_RESULT Parameter(WORK *Work, UINT8 *Packet, UINT32 *Bytes, UINT32 Instance,
                                     UINT32 Id, VIEW Payload)
{
  UINT32 Padding = (8U - (Payload.Bytes & 7U)) & 7U;
  if (Payload.Bytes > CR_AUDIO_CAL_MAX_PACKET_BYTES - 16U - Padding ||
      *Bytes > CR_AUDIO_CAL_MAX_PACKET_BYTES - 16U - Padding - Payload.Bytes ||
      Work->Plan.ParameterCount == CR_AUDIO_CAL_MAX_PARAMETERS) {
    return CrAudioCalLimit;
  }
  Write32(Packet + *Bytes, Instance);
  Write32(Packet + *Bytes + 4, Id);
  Write32(Packet + *Bytes + 8, Payload.Bytes);
  Write32(Packet + *Bytes + 12, 0);
  Copy(Packet + *Bytes + 16, Payload.Data, Payload.Bytes);
  Zero(Packet + *Bytes + 16 + Payload.Bytes, Padding);
  *Bytes += 16U + Payload.Bytes + Padding;
  ++Work->Plan.ParameterCount;
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Driver(WORK *Work, UINT32 Id, VIEW Data)
{
  CURSOR Cursor = {Data, 0};
  UINT32 FoundId, Count, GenericCount = 0, TypeCount = 0;
  INT32 Index = SubgraphIndex(Work, Id);
  if (Index < 0) {
    return CrAudioCalMalformed;
  }
  TRY(Word(&Cursor, &FoundId));
  TRY(Word(&Cursor, &Count));
  if (FoundId != Id || Count > (Data.Bytes - Cursor.Position) / 8) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Property = 0; Property < Count; ++Property) {
    UINT32 Pid, Bytes;
    VIEW Value;
    TRY(Word(&Cursor, &Pid));
    TRY(Word(&Cursor, &Bytes));
    TRY(Take(&Cursor, Bytes, &Value));
    if (Pid == 0x08000001U) {
      if (++GenericCount != 1 || Bytes != 8) {
        return CrAudioCalMalformed;
      }
      Work->Plan.Subgraphs[Index].RoutingDomain = Read32(Value.Data);
      Work->Plan.Subgraphs[Index].Flushable = Read32(Value.Data + 4);
      if (Read32(Value.Data) != 2) {
        return CrAudioCalUnsupportedDomain;
      }
      if (Read32(Value.Data + 4) > 1) {
        return CrAudioCalMalformed;
      }
    } else if (Pid == 0x08000010U) {
      if (++TypeCount != 1 || Bytes != 4) {
        return CrAudioCalMalformed;
      }
      Work->Plan.Subgraphs[Index].Type = Read32(Value.Data);
      if (Read32(Value.Data) < 1 || Read32(Value.Data) > 5) {
        return CrAudioCalUnsupportedDriver;
      }
    } else {
      return CrAudioCalUnsupportedDriver;
    }
  }
  if (GenericCount != 1 || Cursor.Position != Data.Bytes) {
    return CrAudioCalMalformed;
  }
  if (Data.Bytes > CR_AUDIO_CAL_MAX_DRIVER_BYTES - Work->Plan.DriverBytes) {
    return CrAudioCalLimit;
  }
  Copy(Work->Plan.Driver + Work->Plan.DriverBytes, Data.Data, Data.Bytes);
  Work->Plan.DriverBytes += Data.Bytes;
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT ModuleDeclarations(WORK *Work, UINT32 Sgid, VIEW Data)
{
  CURSOR Cursor = {Data, 0};
  UINT32 Count;
  TRY(Word(&Cursor, &Count));
  if (Count > (Data.Bytes - Cursor.Position) / 12) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < Count; ++Index) {
    UINT32 Id, Container, Modules;
    VIEW List;
    TRY(Word(&Cursor, &Id));
    TRY(Word(&Cursor, &Container));
    TRY(Word(&Cursor, &Modules));
    if (Id != Sgid || Container == 0) {
      return CrAudioCalMalformed;
    }
    TRY(Array(&Cursor, Modules, 8, &List));
    for (UINT32 Item = 0; Item < Modules; ++Item) {
      UINT32 Mid = Read32(List.Data + Item * 8);
      UINT32 Iid = Read32(List.Data + Item * 8 + 4);
      INT32 Found = ModuleIndex(Work, Iid);
      if (Found < 0 || Work->Plan.Modules[Found].Subgraph != Id ||
          Work->Plan.Modules[Found].Module != Mid || Work->Declared[Found] != 0) {
        return CrAudioCalMalformed;
      }
      Work->Declared[Found] = 1;
      Work->Plan.Modules[Found].Container = Container;
    }
  }
  return Cursor.Position == Data.Bytes ? CrAudioCalOk : CrAudioCalMalformed;
}
/* Validate variable-length APM configuration structures before copying them.
   Known leaf properties are checked against the pinned public APM API. */
static CR_AUDIO_CAL_RESULT PropertyItems(CURSOR *Cursor, UINT32 Count, UINT32 Kind)
{
  UINT32 Seen[32] = {0};
  if (Count > 32) {
    return CrAudioCalLimit;
  }
  for (UINT32 Index = 0; Index < Count; ++Index) {
    UINT32 Id, Bytes;
    VIEW Value;
    TRY(Word(Cursor, &Id));
    TRY(Word(Cursor, &Bytes));
    TRY(Take(Cursor, Bytes, &Value));
    for (UINT32 Prior = 0; Prior < Index; ++Prior) {
      if (Seen[Prior] == Id) {
        return CrAudioCalMalformed;
      }
    }
    Seen[Index] = Id;
    UINT32 Required = 4;
    if (Kind == 0x08001000U) {
      if (Id == 0x08001011U) {
        Required = 8;
      } else if (Id != 0x08001012U && Id != 0x08001013U && Id != 0x08001014U && Id != 0x080010cbU &&
                 Id != 0x08001174U && Id != 0x0800124dU) {
        return CrAudioCalUnsupportedGraphProperty;
      }
    } else if (Kind == 0x08001001U) {
      if (Id != 0x0800100eU && Id != 0x0800100fU && Id != 0x08001010U && Id != 0x080010ccU) {
        return CrAudioCalUnsupportedGraphProperty;
      }
    } else if (Kind == 0x08001003U) {
      if (Id != 0x08001015U) {
        return CrAudioCalUnsupportedGraphProperty;
      }
      Required = 8;
    } else {
      if (Id != 0x08001062U) {
        return CrAudioCalUnsupportedGraphProperty;
      }
      VIEW Intents;
      TRY(Counted(Value, 0, 4, &Intents));
      if (Intents.Bytes + 4 != Bytes) {
        return CrAudioCalMalformed;
      }
      continue;
    }
    if (Bytes != Required) {
      return CrAudioCalMalformed;
    }
    if (Kind == 0x08001000U) {
      if (Id == 0x08001014U && Read32(Value.Data) != 2 && Read32(Value.Data) != 0xffffffffU) {
        return CrAudioCalUnsupportedDomain;
      }
      if (Id == 0x080010cbU && Read32(Value.Data) != 0xffffffffU) {
        return CrAudioCalUnsupportedOffload;
      }
      if (Id == 0x08001011U && Read32(Value.Data) != 1) {
        return CrAudioCalUnsupportedGraphProperty;
      }
    }
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT ConfigPayload(WORK *Work, UINT32 Sgid, UINT32 Pid, VIEW Payload)
{
  CURSOR Cursor = {Payload, 0};
  UINT32 Count;
  TRY(Word(&Cursor, &Count));
  if (Pid == 0x08001004U) {
    VIEW Links;
    TRY(Array(&Cursor, Count, 16, &Links));
    /* Real graphs contain optional links to modules outside the chosen SG
       set (for example an echo reference). Preserve them for APM; this
       offline compiler does not claim those external modules are opened. */
    for (UINT32 Index = 0; Index < Count; ++Index) {
      CONST UINT8 *Link = Links.Data + Index * 16;
      if (Read32(Link) == 0 || Read32(Link + 4) == 0 || Read32(Link + 8) == 0 ||
          Read32(Link + 12) == 0) {
        return CrAudioCalMalformed;
      }
    }
  } else {
    if (Count > Payload.Bytes / 8) {
      return CrAudioCalMalformed;
    }
    for (UINT32 Index = 0; Index < Count; ++Index) {
      UINT32 Id, Properties;
      TRY(Word(&Cursor, &Id));
      if (Pid == 0x08001061U) {
        VIEW Peers;
        TRY(Take(&Cursor, 12, &Peers));
        if (Id == 0 || Read32(Peers.Data) == 0 || Read32(Peers.Data + 4) == 0 ||
            Read32(Peers.Data + 8) == 0) {
          return CrAudioCalMalformed;
        }
      } else if (Pid == 0x08001001U) {
        if (Count != 1 || Id != Sgid) {
          return CrAudioCalMalformed;
        }
      } else if (Pid == 0x08001003U) {
        INT32 Module = ModuleIndex(Work, Id);
        if (Module < 0 || Work->Plan.Modules[Module].Subgraph != Sgid) {
          return CrAudioCalMalformed;
        }
      } else if (Id == 0) {
        return CrAudioCalMalformed;
      }
      TRY(Word(&Cursor, &Properties));
      TRY(PropertyItems(&Cursor, Properties, Pid));
    }
  }
  return Cursor.Position == Payload.Bytes ? CrAudioCalOk : CrAudioCalMalformed;
}

/* Open property payloads retain source bytes. Nested module declarations are
   checked against SGIT; arbitrary DSP module calibration is not decoded here. */
static CR_AUDIO_CAL_RESULT Properties(WORK *Work, UINT32 Offset)
{
  VIEW Object;
  TRY(PoolObject(Work, Offset, &Object));
  CURSOR Cursor = {Object, 0};
  UINT32 Count, Seen[CR_AUDIO_CAL_MAX_SUBGRAPHS] = {0};
  TRY(Word(&Cursor, &Count));
  if (Count != Work->Plan.SubgraphCount) {
    return CrAudioCalMalformed;
  }
  /* Output order is graph order, matching public GET_SUBGRAPH_DATA. */
  for (UINT32 Wanted = 0; Wanted < Count; ++Wanted) {
    CURSOR Objects = Cursor;
    BOOLEAN Found = FALSE;
    for (UINT32 Index = 0; Index < Count; ++Index) {
      UINT32 Id, Bytes;
      VIEW Body;
      TRY(Word(&Objects, &Id));
      TRY(Word(&Objects, &Bytes));
      TRY(Take(&Objects, Bytes, &Body));
      INT32 Sg = SubgraphIndex(Work, Id);
      if (Sg < 0) {
        return CrAudioCalMalformed;
      }
      if (Wanted == 0 && ++Seen[Sg] != 1) {
        return CrAudioCalMalformed;
      }
      if (Id != Work->Plan.Subgraphs[Wanted].Id) {
        continue;
      }
      Found = TRUE;
      CURSOR Sections = {Body, 0};
      VIEW DriverData, Spf;
      TRY(Word(&Sections, &Bytes));
      TRY(Take(&Sections, Bytes, &DriverData));
      TRY(Driver(Work, Id, DriverData));
      TRY(Word(&Sections, &Bytes));
      TRY(Take(&Sections, Bytes, &Spf));
      if (Sections.Position != Body.Bytes) {
        return CrAudioCalMalformed;
      }
      CURSOR Parameters = {Spf, 0};
      UINT32 PropertyMask = 0;
      while (Parameters.Position < Spf.Bytes) {
        UINT32 Iid, Pid, Size;
        VIEW Payload, Padding;
        TRY(Word(&Parameters, &Iid));
        TRY(Word(&Parameters, &Pid));
        TRY(Word(&Parameters, &Size));
        TRY(Take(&Parameters, Size, &Payload));
        TRY(Take(&Parameters, (8U - (Size & 7U)) & 7U, &Padding));
        if (Iid != 1) {
          return CrAudioCalUnsupportedGraphProperty;
        }
        if (Pid >= 0x08001000U && Pid <= 0x08001004U) {
          UINT32 Bit = 1U << (Pid - 0x08001000U);
          if ((PropertyMask & Bit) != 0) {
            return CrAudioCalMalformed;
          }
          PropertyMask |= Bit;
        } else if (Pid != 0x08001061U) {
          return CrAudioCalUnsupportedGraphProperty;
        }
        if (Pid == 0x08001002U) {
          TRY(ModuleDeclarations(Work, Id, Payload));
        } else {
          TRY(ConfigPayload(Work, Id, Pid, Payload));
        }
        TRY(Parameter(Work, Work->Plan.Open, &Work->Plan.OpenBytes, Iid, Pid, Payload));
      }
      if ((PropertyMask & 7U) != 7U) {
        return CrAudioCalMalformed;
      }
    }
    if (!Found || Objects.Position != Object.Bytes) {
      return CrAudioCalMalformed;
    }
  }
  for (UINT32 Index = 0; Index < Work->Plan.ModuleCount; ++Index) {
    if (Work->Declared[Index] != 1) {
      return CrAudioCalMalformed;
    }
  }
  Work->Plan.OpenPropertyBytes = Work->Plan.OpenBytes - 40;
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Definition(WORK *Work, UINT32 DefChunk, UINT32 DotChunk,
                                      UINT32 DefOffset, UINT32 DotOffset, VIEW *Definitions,
                                      VIEW *Offsets)
{
  TRY(Counted(Work->Chunks[DefChunk], DefOffset, 8, Definitions));
  TRY(Counted(Work->Chunks[DotChunk], DotOffset, 4, Offsets));
  if (Definitions->Bytes / 8 != Offsets->Bytes / 4) {
    return CrAudioCalMalformed;
  }
  if (Definitions->Bytes / 8 > CR_AUDIO_CAL_MAX_PARAMETERS) {
    return CrAudioCalLimit;
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Connections(WORK *Work)
{
  VIEW Table, Match;
  TRY(Counted(Work->Chunks[Sclu], 0, 16, &Table));
  if (Table.Bytes + 4 != Work->Chunks[Sclu].Bytes) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < Work->Plan.EdgeCount; ++Index) {
    UINT8 Keys[8];
    VIEW Defs, Dots;
    Write32(Keys, Work->Plan.Edges[Index].Source);
    Write32(Keys + 4, Work->Plan.Edges[Index].Destination);
    TRY(Rows(Table, 2, 2, Keys, &Match));
    TRY(Definition(Work, Scde, Scdo, Read32(Match.Data), Read32(Match.Data + 4), &Defs, &Dots));
    for (UINT32 Param = 0; Param < Dots.Bytes / 4; ++Param) {
      UINT32 Iid = Read32(Defs.Data + Param * 8);
      UINT32 Pid = Read32(Defs.Data + Param * 8 + 4);
      VIEW Payload;
      if (Iid != 1 || (Pid != 0x08001004U && Pid != 0x08001061U)) {
        return CrAudioCalUnsupportedGraphProperty;
      }
      TRY(PoolObject(Work, Read32(Dots.Data + Param * 4), &Payload));
      TRY(ConfigPayload(Work, 0, Pid, Payload));
      TRY(Parameter(Work, Work->Plan.Open, &Work->Plan.OpenBytes, Iid, Pid, Payload));
    }
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT FeatureLists(WORK *Work)
{
  /* Conservative initial scope: nonempty AMDB/driver/persistent/hardware
     offload maps require their owners before any complete plan is emitted. */
  CONST UINT32 Unsupported[] = {Modm, Btup, Gclu, Gppm, Hasi};
  for (UINT32 Index = 0; Index < sizeof(Unsupported) / sizeof(Unsupported[0]); ++Index) {
    UINT32 Chunk = Unsupported[Index];
    VIEW Data = Work->Chunks[Chunk];
    if (Data.Data == NULL) {
      continue;
    }
    if (Data.Bytes < 4) {
      return CrAudioCalMalformed;
    }
    if (Read32(Data.Data) != 0) {
      if (Chunk == Modm || Chunk == Btup) {
        return CrAudioCalUnsupportedAmdb;
      }
      if (Chunk == Gppm) {
        return CrAudioCalUnsupportedPersistence;
      }
      if (Chunk == Hasi) {
        return CrAudioCalUnsupportedOffload;
      }
      return CrAudioCalUnsupportedDriver;
    }
    if (Data.Bytes != 4) {
      return CrAudioCalMalformed;
    }
  }
  VIEW Persistent;
  TRY(Counted(Work->Chunks[Pptc], 0, 4, &Persistent));
  if (Persistent.Bytes + 4 != Work->Chunks[Pptc].Bytes) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 4; Index < Persistent.Bytes; Index += 4) {
    if (Read32(Persistent.Data + Index - 4) >= Read32(Persistent.Data + Index)) {
      return CrAudioCalMalformed;
    }
  }
  VIEW Global = Work->Chunks[Gpro];
  if (Global.Data != NULL) {
    VIEW Table, Match;
    UINT8 Key[4];
    Write32(Key, 0x000133b7U);
    TRY(Counted(Global, 0, 8, &Table));
    if (Table.Bytes + 4 != Global.Bytes) {
      return CrAudioCalMalformed;
    }
    CR_AUDIO_CAL_RESULT Result = Rows(Table, 1, 1, Key, &Match);
    if (Result == CrAudioCalOk) {
      VIEW Object;
      TRY(PoolObject(Work, Read32(Match.Data), &Object));
      TRY(Counted(Object, 0, 4, &Work->Offloaded));
      if (Work->Offloaded.Bytes + 4 != Object.Bytes) {
        return CrAudioCalMalformed;
      }
    } else if (Result != CrAudioCalNotFound) {
      return Result;
    }
  }
  return CrAudioCalOk;
}
static BOOLEAN Contains(VIEW List, UINT32 Value)
{
  for (UINT32 Offset = 0; Offset < List.Bytes; Offset += 4) {
    if (Read32(List.Data + Offset) == Value) {
      return TRUE;
    }
  }
  return FALSE;
}
static CR_AUDIO_CAL_RESULT CalibrationRow(WORK *Work, CONST UINT8 *Entry, VIEW *Definitions,
                                          VIEW *Offsets, BOOLEAN *Default)
{
  VIEW KeyIds, Table, Match;
  UINT8 Values[CR_AUDIO_CAL_MAX_KEYS * 4] = {0};
  UINT32 Keys, Found = 0, Used = 0;
  TRY(Counted(Work->Chunks[Cakt], Read32(Entry), 4, &KeyIds));
  Keys = KeyIds.Bytes / 4;
  if (Keys > CR_AUDIO_CAL_MAX_KEYS) {
    return CrAudioCalLimit;
  }
  *Default = Keys == 0;
  for (UINT32 Index = 0; Index < Keys; ++Index) {
    UINT32 Key = Read32(KeyIds.Data + Index * 4);
    if (Index != 0 && Read32(KeyIds.Data + (Index - 1) * 4) >= Key) {
      return CrAudioCalMalformed;
    }
    for (UINT32 Request = 0; Request < Work->Plan.Request.CalibrationKeyCount; ++Request) {
      if (Work->Plan.Request.CalibrationKeys[Request].Key == Key) {
        Write32(Values + Index * 4, Work->Plan.Request.CalibrationKeys[Request].Value);
        ++Found;
        Used |= 1U << Request;
        break;
      }
    }
  }
  CURSOR Cursor = {Work->Chunks[Cdlu], Read32(Entry + 4)};
  UINT32 Count, Entries;
  if (Cursor.Position > Cursor.View.Bytes) {
    return CrAudioCalMalformed;
  }
  TRY(Word(&Cursor, &Count));
  TRY(Word(&Cursor, &Entries));
  if (Count != Keys || Entries == 0) {
    return CrAudioCalMalformed;
  }
  TRY(Array(&Cursor, Entries, (Keys + 3) * 4, &Table));
  CR_AUDIO_CAL_RESULT Result = Rows(Table, Keys, 3, Values, &Match);
  if (Result != CrAudioCalOk && Result != CrAudioCalNotFound) {
    return Result;
  }
  if (Found != Keys) {
    /* Upstream's initial partially specified CKV can search uninitialized
       global values. Reject it; do not copy that behavior into a new driver. */
    return Found == 0 ? CrAudioCalNotFound : CrAudioCalMissingCalibrationKey;
  }
  if (Result == CrAudioCalNotFound) {
    return CrAudioCalMissingCalibrationKey;
  }
  Work->UsedKeys |= Used;
  return Definition(Work, Cdde, Cddo, Read32(Match.Data), Read32(Match.Data + 4), Definitions,
                    Offsets);
}
static CR_AUDIO_CAL_RESULT CalibrationData(WORK *Work, UINT32 Sgid, VIEW Defs, VIEW Dots,
                                           BOOLEAN Collect, BOOLEAN Default)
{
  VIEW Persistent;
  TRY(Counted(Work->Chunks[Pptc], 0, 4, &Persistent));
  for (UINT32 Index = 0; Index < Defs.Bytes / 8; ++Index) {
    UINT32 Iid = Read32(Defs.Data + Index * 8);
    UINT32 Pid = Read32(Defs.Data + Index * 8 + 4);
    INT32 Module = ModuleIndex(Work, Iid);
    VIEW Payload;
    if (Module < 0 || Work->Plan.Modules[Module].Subgraph != Sgid || Pid == 0) {
      return CrAudioCalMalformed;
    }
    if (Index != 0 && Compare(Defs.Data + (Index - 1) * 8, Defs.Data + Index * 8, 2) >= 0) {
      return CrAudioCalMalformed;
    }
    TRY(PoolObject(Work, Read32(Dots.Data + Index * 4), &Payload));
    if (Contains(Persistent, Pid)) {
      return CrAudioCalUnsupportedPersistence;
    }
    if (Contains(Work->Offloaded, Pid)) {
      return CrAudioCalUnsupportedOffload;
    }
    if (Collect) {
      UINT32 Reference = 0;
      while (Reference < Work->ReferenceCount && Work->References[Reference].Instance != Iid) {
        ++Reference;
      }
      if (Reference == Work->ReferenceCount) {
        if (Reference == CR_AUDIO_CAL_MAX_PARAMETERS) {
          return CrAudioCalLimit;
        }
        Work->References[Reference].Instance = Iid;
        Work->References[Reference].Count = 0;
        ++Work->ReferenceCount;
      }
      ++Work->References[Reference].Count;
      continue;
    }
    BOOLEAN Found = FALSE;
    for (UINT32 Reference = 0; Reference < Work->ReferenceCount; ++Reference) {
      if (Work->References[Reference].Instance == Iid) {
        if (Work->References[Reference].Count != 0) {
          --Work->References[Reference].Count;
          Found = TRUE;
        }
        break;
      }
    }
    if (Default && Work->ReferenceCount != 0 && !Found) {
      continue;
    }
    TRY(Parameter(Work, Work->Plan.Calibration, &Work->Plan.CalibrationBytes, Iid, Pid, Payload));
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Calibration(WORK *Work)
{
  UINT32 Sorted[CR_AUDIO_CAL_MAX_SUBGRAPHS];
  for (UINT32 Index = 0; Index < Work->Plan.SubgraphCount; ++Index) {
    Sorted[Index] = Work->Plan.Subgraphs[Index].Id;
    UINT32 Position = Index;
    while (Position != 0 && Sorted[Position - 1] > Sorted[Position]) {
      UINT32 Value = Sorted[Position];
      Sorted[Position] = Sorted[Position - 1];
      Sorted[--Position] = Value;
    }
  }
  /* Upstream mutates the caller's SG array to ascending order; this library
     sorts a private copy and retains graph topology order in its output. */
  for (UINT32 Wanted = 0; Wanted < Work->Plan.SubgraphCount; ++Wanted) {
    CURSOR Cursor = {Work->Chunks[Cslu], 0};
    UINT32 Count, Prior = 0;
    BOOLEAN Found = FALSE;
    TRY(Word(&Cursor, &Count));
    if (Count > (Cursor.View.Bytes - Cursor.Position) / 8) {
      return CrAudioCalMalformed;
    }
    for (UINT32 Index = 0; Index < Count; ++Index) {
      UINT32 Id, Entries;
      VIEW Tables;
      TRY(Word(&Cursor, &Id));
      TRY(Word(&Cursor, &Entries));
      if (Index != 0 && Id <= Prior) {
        return CrAudioCalMalformed;
      }
      Prior = Id;
      TRY(Array(&Cursor, Entries, 8, &Tables));
      if (Id != Sorted[Wanted]) {
        continue;
      }
      Found = TRUE;
      if (Entries == 0) {
        return CrAudioCalMissingCalibration;
      }
      VIEW DefaultDefs, DefaultDots;
      BOOLEAN IsDefault;
      Work->ReferenceCount = 0;
      TRY(CalibrationRow(Work, Tables.Data, &DefaultDefs, &DefaultDots, &IsDefault));
      if (!IsDefault) {
        return CrAudioCalMalformed;
      }
      if (Entries > 1) {
        TRY(CalibrationData(Work, Id, DefaultDefs, DefaultDots, TRUE, TRUE));
        for (UINT32 Specific = 1; Specific < Entries; ++Specific) {
          VIEW Defs, Dots;
          CR_AUDIO_CAL_RESULT Result =
              CalibrationRow(Work, Tables.Data + Specific * 8, &Defs, &Dots, &IsDefault);
          if (Result == CrAudioCalNotFound) {
            continue;
          }
          if (Result != CrAudioCalOk) {
            return Result;
          }
          if (IsDefault) {
            return CrAudioCalMalformed;
          }
          TRY(CalibrationData(Work, Id, Defs, Dots, FALSE, FALSE));
        }
      }
      TRY(CalibrationData(Work, Id, DefaultDefs, DefaultDots, FALSE, TRUE));
    }
    if (Cursor.Position != Cursor.View.Bytes) {
      return CrAudioCalMalformed;
    }
    if (!Found) {
      return CrAudioCalMissingCalibration;
    }
  }
  UINT32 Expected = Work->Plan.Request.CalibrationKeyCount == 32
                        ? 0xffffffffU
                        : ((1U << Work->Plan.Request.CalibrationKeyCount) - 1U);
  if (Work->UsedKeys != Expected) {
    return CrAudioCalMissingCalibrationKey;
  }
  return Work->Plan.CalibrationBytes > 40 ? CrAudioCalOk : CrAudioCalMissingCalibration;
}
static CR_AUDIO_CAL_RESULT TaggedEndpoint(WORK *Work, UINT32 Tag, UINT32 ExpectedMid, UINT32 *Instance)
{
  VIEW Table, Match;
  UINT32 Found = 0;
  TRY(Counted(Work->Chunks[Tmlu], 0, 12, &Table));
  if (Table.Bytes + 4 != Work->Chunks[Tmlu].Bytes) {
    return CrAudioCalMalformed;
  }
  for (UINT32 Index = 0; Index < Work->Plan.SubgraphCount; ++Index) {
    UINT8 Keys[8];
    Write32(Keys, Work->Plan.Subgraphs[Index].Id);
    Write32(Keys + 4, Tag);
    CR_AUDIO_CAL_RESULT Result = Rows(Table, 2, 1, Keys, &Match);
    if (Result == CrAudioCalNotFound) {
      continue;
    }
    if (Result != CrAudioCalOk) {
      return Result;
    }
    VIEW Modules;
    TRY(Counted(Work->Chunks[Tmde], Read32(Match.Data), 8, &Modules));
    if (Modules.Bytes == 0) {
      continue;
    }
    if (Modules.Bytes != 8 || ++Found != 1) {
      return CrAudioCalUnsupportedEndpoint;
    }
    UINT32 Mid = Read32(Modules.Data), Iid = Read32(Modules.Data + 4);
    INT32 Module = ModuleIndex(Work, Iid);
    if (Module < 0 || Work->Plan.Modules[Module].Module != Mid ||
        Work->Plan.Modules[Module].Subgraph != Work->Plan.Subgraphs[Index].Id) {
      return CrAudioCalMalformed;
    }
    if (Mid != ExpectedMid) {
      return CrAudioCalUnsupportedEndpoint;
    }
    *Instance = Iid;
  }
  if (Found != 1) {
    return CrAudioCalUnsupportedEndpoint;
  }
  return CrAudioCalOk;
}
static CR_AUDIO_CAL_RESULT Endpoint(WORK *Work)
{
  TRY(TaggedEndpoint(Work, Work->Plan.Request.EndpointTag, 0x07001023U, &Work->Plan.EndpointInstance));
  Work->Plan.EndpointModule = 0x07001023U;
  UINT8 Config[12];
  VIEW Payload = {Config, sizeof(Config)};
  Write32(Config, Work->Plan.Request.LpaifType);
  Write32(Config + 4, Work->Plan.Request.InterfaceIndex);
  Write32(Config + 8, Work->Plan.Request.ActiveChannelsMask);
  /* Two records are exactly 64 bytes. Parameter's generic packet cap is safe
     here because EndpointBytes starts at40 and these sizes are fixed. */
  TRY(Parameter(Work, Work->Plan.Endpoint, &Work->Plan.EndpointBytes, Work->Plan.EndpointInstance,
                0x08001063U, Payload));
  Write32(Config, Work->Plan.Request.SampleRate);
  Write32(Config + 4, Work->Plan.Request.BitWidth | (Work->Plan.Request.Channels << 16));
  Write32(Config + 8, 1); /* DATA_FORMAT_FIXED_POINT */
  TRY(Parameter(Work, Work->Plan.Endpoint, &Work->Plan.EndpointBytes, Work->Plan.EndpointInstance,
                0x08001017U, Payload));
  return CrAudioCalOk;
}
/* The selected route is explicit PCM playback to HEADPHONES. Neither this
   list nor additional exact GKV entries claim a particular HDK tuning row. */
CR_AUDIO_CAL_RESULT CrAudioCalValidateHeadphoneRequest(CONST CR_AUDIO_CAL_REQUEST *Request)
{
  if (!Range(Request, sizeof(*Request)) || ((UINTN)Request & 3U) != 0 ||
      !Request->GraphKeyCount || Request->GraphKeyCount > CR_AUDIO_CAL_MAX_KEYS) {
    return CrAudioCalInvalidArgument;
  }
  UINT32 Stream = 0, Device = 0;
  for (UINT32 I = 0; I < Request->GraphKeyCount; ++I) {
    CONST CR_AUDIO_CAL_KEY *K = &Request->GraphKeys[I];
    for (UINT32 J = 0; J < I; ++J) {
      if (Request->GraphKeys[J].Key == K->Key) return CrAudioCalInvalidArgument;
    }
    if (K->Key == 0xa1000000U) Stream = K->Value;
    if (K->Key == 0xa2000000U) Device = K->Value;
  }
  if (Device != 0xa2000002U || Request->EndpointTag != 0xc0000004U ||
      Request->SampleRate != 48000 || Request->BitWidth != 16 || Request->Channels != 2) {
    return CrAudioCalUnsupportedEndpoint;
  }
  switch (Stream) {
  case 0xa1000001U: case 0xa100000eU: case 0xa100000fU:
  case 0xa1000011U: case 0xa100001dU: case 0xa100001eU:
    return CrAudioCalOk;
  default:
    return CrAudioCalUnsupportedEndpoint;
  }
}
static CR_AUDIO_CAL_RESULT Playback(WORK *Work)
{
  TRY(TaggedEndpoint(Work, 0xc0000001U, 0x07001006U, &Work->Playback.MemoryInstance));
  Work->Playback.MemoryModule = 0x07001006U;
  Work->Playback.CodecModule = Work->Plan.EndpointModule;
  Work->Playback.CodecInstance = Work->Plan.EndpointInstance;
  /* APM_MODULE_CONNECTION_CFG is (source MIID, source output port,
     destination MIID, destination input port). Its ports are strictly 1:1.
     Preserve actual source records; no SCLU/port links are synthesized. */
  CURSOR Cursor = {{Work->Plan.Open, Work->Plan.OpenBytes}, 40};
  while (Cursor.Position < Cursor.View.Bytes) {
    UINT32 Iid, Pid, Bytes, Error;
    VIEW Data, Padding;
    TRY(Word(&Cursor, &Iid)); TRY(Word(&Cursor, &Pid));
    TRY(Word(&Cursor, &Bytes)); TRY(Word(&Cursor, &Error));
    TRY(Take(&Cursor, Bytes, &Data));
    TRY(Take(&Cursor, (8U - (Bytes & 7U)) & 7U, &Padding));
    if (Iid != 1 || Error) return CrAudioCalMalformed;
    if (Pid != 0x08001004U) continue;
    VIEW Links;
    TRY(Counted(Data, 0, 16, &Links));
    if (Links.Bytes + 4 != Data.Bytes) return CrAudioCalMalformed;
    for (UINT32 At = 0; At < Links.Bytes; At += 16) {
      CONST UINT8 *L = Links.Data + At;
      INT32 From = ModuleIndex(Work, Read32(L)), To = ModuleIndex(Work, Read32(L + 8));
      if (From < 0 || To < 0) return CrAudioCalUnsupportedExternalGraph;
      DATA_LINK Link = {(UINT32)From, Read32(L + 4), (UINT32)To, Read32(L + 12)};
      if (!Link.SourcePort || !Link.DestinationPort) return CrAudioCalMalformed;
      for (UINT32 I = 0; I < Work->DataLinkCount; ++I) {
        CONST DATA_LINK *Prior = &Work->DataLinks[I];
        if ((Prior->Source == Link.Source && Prior->SourcePort == Link.SourcePort) ||
            (Prior->Destination == Link.Destination && Prior->DestinationPort == Link.DestinationPort)) {
          return CrAudioCalMalformed;
        }
      }
      if (Work->DataLinkCount == CR_AUDIO_CAL_MAX_EDGES) return CrAudioCalLimit;
      Work->DataLinks[Work->DataLinkCount++] = Link;
    }
  }
  UINT8 Reached[CR_AUDIO_CAL_MAX_MODULES] = {0};
  INT32 From = ModuleIndex(Work, Work->Playback.MemoryInstance);
  INT32 To = ModuleIndex(Work, Work->Playback.CodecInstance);
  if (From < 0 || To < 0 || From == To) return CrAudioCalMalformed;
  Reached[From] = 1;
  /* A cycle is not itself forbidden by the ABI. Fixed-point reachability is
     bounded by the actual owned module count and never traverses externals. */
  for (UINT32 Pass = 0; Pass < Work->Plan.ModuleCount; ++Pass) {
    BOOLEAN Changed = FALSE;
    for (UINT32 I = 0; I < Work->DataLinkCount; ++I) {
      CONST DATA_LINK *L = &Work->DataLinks[I];
      if (Reached[L->Source] && !Reached[L->Destination]) {
        Reached[L->Destination] = 1; Changed = TRUE;
      }
    }
    if (!Changed) break;
  }
  return Reached[To] ? CrAudioCalOk : CrAudioCalUnsupportedEndpoint;
}
static VOID PacketHeader(UINT8 *Packet, UINT32 Bytes, UINT32 HostPort, UINT32 Token, UINT32 Opcode)
{
  Write32(Packet, (Bytes << 8) | (6U << 4));
  Packet[4] = 2;
  Packet[5] = 3;
  Write32(Packet + 8, HostPort);
  Write32(Packet + 12, 1);
  Write32(Packet + 16, Token);
  Write32(Packet + 20, Opcode);
  /* AddressLSW, AddressMSW and mapHandle remain zero for in-band payload. */
  Write32(Packet + 36, Bytes - 40);
}
UINTN CrAudioCalWorkspaceBytes(VOID)
{
  return sizeof(WORK);
}
static CR_AUDIO_CAL_RESULT BuildPlan(CONST VOID *Data, UINTN Bytes,
    CONST CR_AUDIO_CAL_REQUEST *Request, VOID *Workspace, UINTN WorkspaceBytes,
    CR_AUDIO_CAL_PLAN *Plan, CR_AUDIO_CAL_PLAYBACK_ENDPOINTS *Endpoints)
{
  CONST VOID *Regions[5] = {Data, Request, Workspace, Plan, Endpoints};
  UINTN Sizes[5] = {Bytes, sizeof(*Request), WorkspaceBytes, sizeof(*Plan), sizeof(*Endpoints)};
  for (UINT32 Index = 0; Index < (Endpoints ? 5U : 4U); ++Index) {
    if (!Range(Regions[Index], Sizes[Index]) || (Index != 0 && ((UINTN)Regions[Index] & 3U) != 0)) {
      return CrAudioCalInvalidArgument;
    }
    for (UINT32 Prior = 0; Prior < Index; ++Prior) {
      if (Overlap(Regions[Index], Sizes[Index], Regions[Prior], Sizes[Prior])) {
        return CrAudioCalInvalidArgument;
      }
    }
  }
  if (((UINTN)Workspace & (sizeof(UINTN) - 1U)) != 0) {
    return CrAudioCalInvalidArgument;
  }
  if (Bytes > 0xffffffffU) {
    return CrAudioCalLimit;
  }
  if (WorkspaceBytes < sizeof(WORK)) {
    return CrAudioCalWorkspaceSmall;
  }
  if (Request->GraphKeyCount == 0 || Request->GraphKeyCount > CR_AUDIO_CAL_MAX_KEYS ||
      Request->CalibrationKeyCount > CR_AUDIO_CAL_MAX_KEYS || Request->HostPort == 0 ||
      Request->HostPort == 0xffffffffU || Request->OpenToken == Request->CalibrationToken ||
      Request->OpenToken == Request->EndpointToken ||
      Request->CalibrationToken == Request->EndpointToken) {
    return CrAudioCalInvalidArgument;
  }
  if (Request->SampleRate != 48000 || Request->BitWidth != 16 || Request->Channels != 2 ||
      Request->LpaifType == 0 || Request->LpaifType > 3) {
    return CrAudioCalUnsupportedEndpoint;
  }
  UINT32 Channels = 0;
  for (UINT32 Mask = Request->ActiveChannelsMask; Mask != 0; Mask >>= 1) {
    Channels += Mask & 1U;
  }
  if (Channels != Request->Channels) {
    return CrAudioCalInvalidArgument;
  }
  if (Endpoints) TRY(CrAudioCalValidateHeadphoneRequest(Request));
  WORK *Work = Workspace;
  Zero(Work, sizeof(*Work));
  Copy(&Work->Plan.Request, Request, sizeof(*Request));
  TRY(SortKeys(Work->Plan.Request.GraphKeys, Request->GraphKeyCount));
  TRY(SortKeys(Work->Plan.Request.CalibrationKeys, Request->CalibrationKeyCount));
  Work->Plan.OpenBytes = Work->Plan.CalibrationBytes = Work->Plan.EndpointBytes = 40;
  TRY(Chunks(Work, Data, Bytes));
  UINT32 Offset;
  TRY(Graph(Work, &Offset));
  TRY(FeatureLists(Work));
  TRY(ModuleMap(Work));
  TRY(Properties(Work, Offset));
  TRY(Connections(Work));
  TRY(Calibration(Work));
  TRY(Endpoint(Work));
  if (Endpoints) TRY(Playback(Work));
  PacketHeader(Work->Plan.Open, Work->Plan.OpenBytes, Request->HostPort, Request->OpenToken,
               0x01001000U);
  PacketHeader(Work->Plan.Calibration, Work->Plan.CalibrationBytes, Request->HostPort,
               Request->CalibrationToken, 0x01001006U);
  PacketHeader(Work->Plan.Endpoint, Work->Plan.EndpointBytes, Request->HostPort,
               Request->EndpointToken, 0x01001006U);
  Copy(Plan, &Work->Plan, sizeof(*Plan));
  if (Endpoints) Copy(Endpoints, &Work->Playback, sizeof(*Endpoints));
  return CrAudioCalOk;
}

CR_AUDIO_CAL_RESULT CrAudioCalBuildPlan(CONST VOID *Data, UINTN Bytes,
    CONST CR_AUDIO_CAL_REQUEST *Request, VOID *Workspace, UINTN WorkspaceBytes,
    CR_AUDIO_CAL_PLAN *Plan)
{
  return BuildPlan(Data, Bytes, Request, Workspace, WorkspaceBytes, Plan, NULL);
}
CR_AUDIO_CAL_RESULT CrAudioCalBuildPullPlan(CONST VOID *Data, UINTN Bytes,
    CONST CR_AUDIO_CAL_REQUEST *Request, VOID *Workspace, UINTN WorkspaceBytes,
    CR_AUDIO_CAL_PLAN *Plan, CR_AUDIO_CAL_PLAYBACK_ENDPOINTS *Endpoints)
{
  if (!Endpoints) return CrAudioCalInvalidArgument;
  return BuildPlan(Data, Bytes, Request, Workspace, WorkspaceBytes, Plan, Endpoints);
}
