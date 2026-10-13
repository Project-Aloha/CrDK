/**
  Qualcomm MDT/MBN ELF32/ELF64 decoding and split segment loading.
  References: Linux drivers/soc/qcom/mdt_loader.c and Qualcomm Android's
  peripheral-loader.c. Wire formats are decoded without unaligned accesses.
  SPDX-License-Identifier: MIT
**/
#include <Library/PILCrLib.h>
#include <oskal/cr_string.h>

#define PIL_HASH_MASK (7U << 24)
#define PIL_HASH (2U << 24)
#define PIL_RELOCATABLE (1U << 27)

STATIC UINT32 Le32(CONST UINT8 *P)
{ return P[0] | ((UINT32)P[1] << 8) | ((UINT32)P[2] << 16) | ((UINT32)P[3] << 24); }
STATIC UINT16 Le16(CONST UINT8 *P)
{ return (UINT16)(P[0] | ((UINT16)P[1] << 8)); }
STATIC UINT64 Le64(CONST UINT8 *P)
{ return Le32(P) | ((UINT64)Le32(P + 4) << 32); }
STATIC BOOLEAN Fits(UINT64 Offset, UINT64 Length, UINT64 Size)
{ return Offset <= Size && Length <= Size - Offset; }

CR_STATUS CrPilInspect(CONST VOID *Header, UINTN Size,
                        CONST CR_PIL_TARGET *Target, CR_PIL_IMAGE *Image)
{
  CONST UINT8 *Bytes = Header, *Ph;
  CR_PIL_SEGMENT *Segment;
  UINT32 Index, Other, PhSize, HeaderSize;
  UINT64 PhOffset, FileSize, MemorySize;
  UINT64 Minimum = CR_MAX_UINT64, Maximum = 0, End, Base;
  BOOLEAN Elf64;
  if (Header == NULL || Target == NULL || Image == NULL)
    return CR_INVALID_PARAMETER;
  cr_memset(Image, 0, sizeof(*Image));
  Image->HashIndex = CR_MAX_UINT32;
  if (Size < 52 || Bytes[0] != 0x7f || Bytes[1] != 'E' ||
      Bytes[2] != 'L' || Bytes[3] != 'F' || (Bytes[4] != 1 && Bytes[4] != 2) ||
      Bytes[5] != 1 || Bytes[6] != 1 || Le32(Bytes + 20) != 1)
    return CR_COMPROMISED_DATA;
  Elf64 = Bytes[4] == 2;
  HeaderSize = Elf64 ? 64 : 52;
  PhSize = Elf64 ? 56 : 32;
  if (Size < HeaderSize || Le16(Bytes + (Elf64 ? 52 : 40)) != HeaderSize ||
      Le16(Bytes + (Elf64 ? 54 : 42)) != PhSize)
    return CR_COMPROMISED_DATA;
  PhOffset = Elf64 ? Le64(Bytes + 32) : Le32(Bytes + 28);
  Image->Count = Le16(Bytes + (Elf64 ? 56 : 44));
  if (PhOffset < HeaderSize || Image->Count < 2 || Image->Count > CR_PIL_MAX_SEGMENTS ||
      !Fits(PhOffset, (UINT64)Image->Count * PhSize, Size) ||
      Target->MemorySize == 0 || (Target->MemoryBase & 4095U) != 0 ||
      (UINT64)Target->MemoryBase + Target->MemorySize > 0x100000000ULL)
    return CR_COMPROMISED_DATA;
  Ph = Bytes + PhOffset;
  FileSize = Elf64 ? Le64(Ph + 32) : Le32(Ph + 16);
  if (Le32(Ph) == 1 || FileSize < PhOffset + Image->Count * PhSize ||
      FileSize > Size || FileSize > CR_PIL_MAX_METADATA)
    return CR_COMPROMISED_DATA;
  Image->HeaderSize = (UINT32)FileSize;
  for (Index = 0; Index < Image->Count; ++Index) {
    Ph = Bytes + PhOffset + Index * PhSize;
    Segment = &Image->Segments[Index];
    Segment->Offset = Elf64 ? Le64(Ph + 8) : Le32(Ph + 4);
    Segment->Address = Elf64 ? Le64(Ph + 24) : Le32(Ph + 12);
    FileSize = Elf64 ? Le64(Ph + 32) : Le32(Ph + 16);
    MemorySize = Elf64 ? Le64(Ph + 40) : Le32(Ph + 20);
    if (FileSize > CR_MAX_UINT32 || MemorySize > CR_MAX_UINT32)
      return CR_COMPROMISED_DATA;
    Segment->FileSize = (UINT32)FileSize;
    Segment->MemorySize = (UINT32)MemorySize;
    Segment->Flags = Le32(Ph + (Elf64 ? 4 : 24));
    if ((Segment->Flags & PIL_HASH_MASK) == PIL_HASH) {
      if (Index == 0 || Image->HashIndex != CR_MAX_UINT32 || Segment->FileSize == 0)
        return CR_COMPROMISED_DATA;
      Image->HashIndex = Index;
      Image->HashSize = Segment->FileSize;
    }
    if (Segment->FileSize != 0 && !Fits(Segment->Offset, Segment->FileSize, Size))
      Image->Split = TRUE;
    Segment->Load = Le32(Ph) == 1 && Segment->MemorySize != 0 &&
                    (Segment->Flags & PIL_HASH_MASK) != PIL_HASH;
    if (!Segment->Load) continue;
    if (Segment->FileSize > Segment->MemorySize)
      return CR_COMPROMISED_DATA;
    if (Segment->Address > CR_MAX_UINT64 - Segment->MemorySize - 4095)
      return CR_COMPROMISED_DATA;
    End = Segment->Address + Segment->MemorySize;
    if ((Segment->Flags & PIL_RELOCATABLE) != 0) Image->Relocatable = TRUE;
    if (Segment->Address < Minimum) Minimum = Segment->Address;
    End = (End + 4095) & ~4095ULL;
    if (End > Maximum) Maximum = End;
  }
  if (Minimum == CR_MAX_UINT64 || Image->HashIndex == CR_MAX_UINT32 ||
      Image->HashSize > CR_PIL_MAX_METADATA - Image->HeaderSize ||
      Maximum - Minimum > Target->MemorySize)
    return CR_COMPROMISED_DATA;
  Image->Span = (UINT32)(Maximum - Minimum);
  Base = Image->Relocatable ? Minimum : Target->MemoryBase;
  for (Index = 0; Index < Image->Count; ++Index) {
    Segment = &Image->Segments[Index];
    if (!Segment->Load) continue;
    if (Segment->Address < Base ||
        !Fits(Segment->Address - Base, Segment->MemorySize, Target->MemorySize))
      return CR_COMPROMISED_DATA;
    Segment->Destination = (UINT32)(Segment->Address - Base);
    for (Other = 0; Other < Index; ++Other) {
      CONST CR_PIL_SEGMENT *Previous = &Image->Segments[Other];
      if (Previous->Load &&
          (UINT64)Segment->Destination < (UINT64)Previous->Destination + Previous->MemorySize &&
          (UINT64)Previous->Destination < (UINT64)Segment->Destination + Segment->MemorySize)
        return CR_COMPROMISED_DATA;
    }
  }
  return CR_SUCCESS;
}

CR_STATUS CrPilBuildMetadata(CONST VOID *Header, UINTN Size,
                              CONST CR_PIL_IMAGE *Image,
                              CR_PIL_READ_SEGMENT Read, VOID *FileContext,
                              VOID *Metadata, UINTN MetadataSize)
{
  UINT64 Offset;
  if (Header == NULL || Image == NULL || Metadata == NULL ||
      Image->HashIndex >= Image->Count || Image->Count > CR_PIL_MAX_SEGMENTS ||
      Image->HeaderSize > Size || Image->HeaderSize > CR_PIL_MAX_METADATA ||
      Image->HashSize > CR_PIL_MAX_METADATA - Image->HeaderSize ||
      MetadataSize != (UINTN)Image->HeaderSize + Image->HashSize)
    return CR_INVALID_PARAMETER;
  cr_memcpy(Metadata, Header, Image->HeaderSize);
  /* Packed MDT places the hash immediately after the header, although the
     original ELF p_offset may describe its position in the unsplit MBN. */
  Offset = Size == MetadataSize ? Image->HeaderSize :
           Image->Segments[Image->HashIndex].Offset;
  if (Fits(Offset, Image->HashSize, Size)) {
    cr_memcpy((UINT8 *)Metadata + Image->HeaderSize,
            (CONST UINT8 *)Header + Offset, Image->HashSize);
    return CR_SUCCESS;
  }
  if (Read == NULL) return CR_NOT_FOUND;
  return Read(FileContext, Image->HashIndex,
              (UINT8 *)Metadata + Image->HeaderSize, Image->HashSize);
}
