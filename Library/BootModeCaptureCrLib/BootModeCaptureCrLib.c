/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <PiPei.h>
#include <Guid/CrBootData.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/BootModeCaptureLib.h>
#include <Library/DebugLib.h>
#include <Library/HobLib.h>
#include <Library/PlatformMemoryMapLib.h>
#include <Library/PrePiLib.h>

#define BOOT_HEAP_SLACK (64U * 1024U)
#define TRANSPORT_DEPTH_MAX 64U

STATIC CR_BOOT_DATA_HOB mBootData;
STATIC BOOLEAN mCaptured;
STATIC BOOLEAN mPublished;

STATIC UINT32 Be32 (CONST UINT8 *P)
{
  return ((UINT32)P[0] << 24) | ((UINT32)P[1] << 16) |
         ((UINT32)P[2] << 8) | P[3];
}

STATIC BOOLEAN Span (UINT64 Base, UINT64 Size)
{
  return Size != 0 && Base != 0 && Base <= MAX_UINT64 - Size;
}

STATIC BOOLEAN Intersects (UINT64 A, UINT64 N, UINT64 B, UINT64 M)
{
  return N != 0 && M != 0 && A < B + M && B < A + N;
}

STATIC BOOLEAN NormalRam (CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *R)
{
  CONST UINT64 Required = EFI_RESOURCE_ATTRIBUTE_PRESENT |
                          EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                          EFI_RESOURCE_ATTRIBUTE_TESTED;
  return ((R->ResourceType == SYS_MEM) || (R->ResourceType == MEM_RES)) &&
         ((R->ResourceAttribute & Required) == Required) &&
         ((R->ResourceAttribute & EFI_RESOURCE_ATTRIBUTE_READ_PROTECTED) == 0) &&
         ((R->ArmAttributes == WRITE_BACK) || (R->ArmAttributes == WRITE_BACK_XN));
}

STATIC BOOLEAN NameIs (CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *R, CONST CHAR8 *Name)
{
  for (UINTN I = 0; I < sizeof (R->Name); ++I) {
    if (R->Name[I] != Name[I]) return FALSE;
    if (Name[I] == 0) return TRUE;
  }
  return FALSE;
}

STATIC BOOLEAN SafeRegion (CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *R)
{
  return NormalRam (R) &&
         (NameIs (R, "DT BLOB") || NameIs (R, "RAM Partition") ||
          NameIs (R, "Kernel") || NameIs (R, "DXE Heap") || NameIs (R, "OEM VM"));
}

#ifdef CR_BOOT_DATA_SMEM
#include "BootDataSmem.inc"
#endif

/* The fixed board map bounds all writes and excludes unsafe read spans.
 * A board-selected SMEM variant can additionally qualify RAM reads. Validate
 * the entire bounded map once; an overlapping unsafe descriptor cannot be
 * hidden behind an earlier permissive name. Unknown DDR is inaccessible. */
STATIC EFI_STATUS MemoryMap (
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX **Map, UINTN *Count, UINTN *Heap)
{
  UINTN I;
  BOOLEAN Found = FALSE;
  *Map = GetPlatformMemoryMap ();
  if (*Map == NULL) return EFI_NOT_READY;
  for (I = 0; I < MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT; ++I) {
    CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *R = &(*Map)[I];
    UINTN NameLength;
    if (R->Length == 0) break;
    if (!Span (R->Address, R->Length)) return EFI_COMPROMISED_DATA;
    for (NameLength = 0; NameLength < sizeof (R->Name) && R->Name[NameLength] != 0;
         ++NameLength) { }
    if (NameLength == sizeof (R->Name)) return EFI_COMPROMISED_DATA;
    if (NameIs (R, "DXE Heap")) {
      CONST UINT64 Required = EFI_RESOURCE_ATTRIBUTE_PRESENT |
                              EFI_RESOURCE_ATTRIBUTE_INITIALIZED |
                              EFI_RESOURCE_ATTRIBUTE_TESTED;
      if (Found || !NormalRam (R) || R->ResourceType != SYS_MEM ||
          R->MemoryType != EfiConventionalMemory || R->HobOption != AddMem ||
          (R->ResourceAttribute & Required) != Required ||
          (R->ResourceAttribute & (EFI_RESOURCE_ATTRIBUTE_READ_PROTECTED |
                                   EFI_RESOURCE_ATTRIBUTE_WRITE_PROTECTED)) != 0)
        return EFI_COMPROMISED_DATA;
      *Heap = I;
      Found = TRUE;
    }
  }
  if (I == MAX_ARM_MEMORY_REGION_DESCRIPTOR_COUNT || !Found)
    return EFI_COMPROMISED_DATA;
  *Count = I;
  return EFI_SUCCESS;
}

STATIC BOOLEAN Readable (
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map, UINTN Count,
  UINT64 Address, UINT64 Size)
{
  UINT64 At, End;
  UINTN I;
  if (!Span (Address, Size)) {
    DEBUG ((DEBUG_ERROR, "BootDataCr: read denied invalid span 0x%Lx+0x%Lx\n",
            Address, Size));
    return FALSE;
  }
  End = Address + Size;
  for (I = 0; I < Count; ++I)
    if (Intersects (Address, Size, Map[I].Address, Map[I].Length) &&
        !SafeRegion (&Map[I])
#ifdef CR_BOOT_DATA_SMEM
        && !SmemBootDataRegion (&Map[I], Address, End)
#endif
        ) {
      DEBUG ((DEBUG_ERROR, "BootDataCr: read denied 0x%Lx+0x%Lx by %a 0x%Lx+0x%Lx\n",
              Address, Size, Map[I].Name, Map[I].Address, Map[I].Length));
      return FALSE;
    }
  At = Address;
  while (At < End) {
    UINT64 Next = At;
    for (I = 0; I < Count; ++I)
      if (SafeRegion (&Map[I]) && Map[I].Address <= At &&
          At - Map[I].Address < Map[I].Length)
        Next = MAX (Next, MIN (End, Map[I].Address + Map[I].Length));
#ifdef CR_BOOT_DATA_SMEM
    if (Next == At) Next = SmemReadableEnd (At, End);
#endif
    if (Next == At) {
      DEBUG ((DEBUG_ERROR, "BootDataCr: read denied 0x%Lx+0x%Lx unqualified RAM at 0x%Lx\n",
              Address, Size, At));
      return FALSE;
    }
    At = Next;
  }
  return TRUE;
}

STATIC BOOLEAN Text (CONST UINT8 *P, UINTN Length, CONST CHAR8 *Want)
{
  UINTN I;
  for (I = 0; I < Length; ++I)
    if (Want[I] == 0 || P[I] != (UINT8)Want[I]) return FALSE;
  return Want[Length] == 0;
}

/* Size includes the literal's NUL. Never scan an arbitrary property string:
 * many properties can share one long string-table entry. Only the two fixed
 * transport keys matter here; DXE validates all other strings and semantics. */
STATIC BOOLEAN PropertyName (
  CONST UINT8 *P, UINTN Available, CONST CHAR8 *Name, UINTN Size)
{
  if (Available < Size) return FALSE;
  for (UINTN I = 0; I < Size; ++I)
    if (P[I] != (UINT8)Name[I]) return FALSE;
  return TRUE;
}

/* Transport metadata exception: a single bounded token walk locates only the
 * direct /chosen initrd endpoints. It does not inspect bootargs, bootconfig
 * magic/checksum/grammar, UFS nodes, phandles or namespace semantics. DXE must
 * independently validate the whole copied FDT before using any of it. */
STATIC EFI_STATUS InitrdTail (
  CONST UINT8 *Fdt, UINTN Size, UINT64 *Original, UINT64 *Length)
{
  UINT32 Struct = Be32 (Fdt + 8), Strings = Be32 (Fdt + 12);
  UINT32 StructSize = Be32 (Fdt + 36), StringsSize = Be32 (Fdt + 32);
  UINTN At, End, Depth = 0;
  UINT64 Begin = 0, Finish = 0;
  BOOLEAN Root = FALSE, Chosen = FALSE, InChosen = FALSE;
  BOOLEAN HasBegin = FALSE, HasEnd = FALSE, Complete = FALSE;
  *Original = *Length = 0;
  if (Struct < 40 || (Struct & 3) || Struct > Size || StructSize > Size - Struct ||
      Strings < 40 || Strings > Size || StringsSize > Size - Strings)
    return EFI_COMPROMISED_DATA;
  At = Struct;
  End = At + StructSize;
  while (At < End) {
    UINT32 Tag;
    if (End - At < 4) return EFI_COMPROMISED_DATA;
    Tag = Be32 (Fdt + At);
    At += 4;
    if (Tag == 1) { /* FDT_BEGIN_NODE */
      UINTN Start = At;
      while (At < End && Fdt[At] != 0) ++At;
      if (At == End || Depth == TRANSPORT_DEPTH_MAX) return EFI_COMPROMISED_DATA;
      if (Depth == 0) {
        if (Root || At != Start) return EFI_COMPROMISED_DATA;
        Root = TRUE;
      } else if (Depth == 1 && Text (Fdt + Start, At - Start, "chosen")) {
        if (Chosen) return EFI_COMPROMISED_DATA;
        Chosen = InChosen = TRUE;
      }
      At = (At + 4) & ~(UINTN)3;
      if (At > End) return EFI_COMPROMISED_DATA;
      ++Depth;
    } else if (Tag == 2) { /* FDT_END_NODE */
      if (Depth == 0) return EFI_COMPROMISED_DATA;
      if (Depth == 2) InChosen = FALSE;
      --Depth;
    } else if (Tag == 3) { /* FDT_PROP */
      UINT32 Bytes, Name;
      if (Depth == 0 || End - At < 8) return EFI_COMPROMISED_DATA;
      Bytes = Be32 (Fdt + At);
      Name = Be32 (Fdt + At + 4);
      At += 8;
      if (Bytes > End - At) return EFI_COMPROMISED_DATA;
      if (InChosen && Depth == 2) {
        BOOLEAN IsBegin, IsEnd;
        if (Name >= StringsSize) return EFI_COMPROMISED_DATA;
        IsBegin = PropertyName (Fdt + Strings + Name, StringsSize - Name,
                               "linux,initrd-start", sizeof ("linux,initrd-start"));
        IsEnd = PropertyName (Fdt + Strings + Name, StringsSize - Name,
                             "linux,initrd-end", sizeof ("linux,initrd-end"));
        if (IsBegin || IsEnd) {
          UINT64 Value;
          if ((Bytes != 4 && Bytes != 8) || (IsBegin && HasBegin) || (IsEnd && HasEnd))
            return EFI_COMPROMISED_DATA;
          Value = Be32 (Fdt + At);
          if (Bytes == 8) Value = (Value << 32) | Be32 (Fdt + At + 4);
          if (IsBegin) { Begin = Value; HasBegin = TRUE; }
          else { Finish = Value; HasEnd = TRUE; }
        }
      }
      At = (At + Bytes + 3) & ~(UINTN)3;
      if (At > End) return EFI_COMPROMISED_DATA;
    } else if (Tag == 9) { /* FDT_END */
      if (!Root || Depth != 0) return EFI_COMPROMISED_DATA;
      Complete = TRUE;
      break;
    } else if (Tag != 4) { /* FDT_NOP */
      return EFI_COMPROMISED_DATA;
    }
  }
  if (!Complete || HasBegin != HasEnd || Finish < Begin)
    return EFI_COMPROMISED_DATA;
  if (!HasBegin || Finish == Begin) return EFI_SUCCESS;
  if (Begin == 0) return EFI_COMPROMISED_DATA;
  if (Finish - Begin < 20) return EFI_SUCCESS;
  *Length = MIN (Finish - Begin, CR_BOOT_DATA_TAIL_MAX);
  *Original = Finish - *Length;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS Arena (
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map, UINTN Count, UINTN Heap,
  UINTN HeapBase, UINTN FreeTop, UINT64 Fdt, UINT64 FdtSize,
  UINT64 Tail, UINT64 TailSize, UINT64 Bytes, UINT64 *Base)
{
  UINT64 Top, Candidate;
  UINTN I;
  if (HeapBase != Map[Heap].Address || FreeTop <= HeapBase ||
      FreeTop > Map[Heap].Address + Map[Heap].Length ||
      HeapBase > MAX_UINT64 - BOOT_HEAP_SLACK) return EFI_INVALID_PARAMETER;
  Top = (UINT64)FreeTop & ~(UINT64)EFI_PAGE_MASK;
  /* At most two source intervals can move the descending candidate. */
  for (UINTN Attempt = 0; Attempt < 3; ++Attempt) {
    UINT64 Next = Top;
    if (Top < Bytes || Top - Bytes < HeapBase + BOOT_HEAP_SLACK)
      return EFI_OUT_OF_RESOURCES;
    Candidate = Top - Bytes;
    if (Intersects (Candidate, Bytes, Fdt, FdtSize)) Next = MIN (Next, Fdt);
    if (Intersects (Candidate, Bytes, Tail, TailSize)) Next = MIN (Next, Tail);
    if (Next != Top) {
      Top = Next & ~(UINT64)EFI_PAGE_MASK;
      continue;
    }
    for (I = 0; I < Count; ++I)
      if (I != Heap && Intersects (Candidate, Bytes, Map[I].Address, Map[I].Length))
        return EFI_COMPROMISED_DATA;
    *Base = Candidate;
    return EFI_SUCCESS;
  }
  return EFI_OUT_OF_RESOURCES;
}

VOID EFIAPI BootModeCapture (
  IN UINTN FdtAddress, IN UINTN HeapBase, IN OUT UINTN *HeapFreeTop)
{
  CONST ARM_MEMORY_REGION_DESCRIPTOR_EX *Map;
  UINTN Count, Heap;
  UINT64 Header[5], Allocation, AllocationSize, Tail, TailSize, TailOffset;
  UINT32 Size;
  EFI_STATUS Status;
  if (mCaptured) return;
  mCaptured = TRUE;
  mPublished = FALSE;
  mBootData = (CR_BOOT_DATA_HOB){
    .Revision = CR_BOOT_DATA_HOB_REVISION, .Size = sizeof (mBootData),
    .FdtStatus = EFI_NOT_READY, .TailStatus = EFI_NOT_READY, .FdtOriginal = FdtAddress
  };
  DEBUG ((DEBUG_ERROR, "BootDataCr: input FDT=0x%Lx heap=0x%Lx top=0x%Lx\n",
          (UINT64)FdtAddress, (UINT64)HeapBase,
          HeapFreeTop != NULL ? (UINT64)*HeapFreeTop : 0));
  if (HeapFreeTop == NULL || FdtAddress == 0 || (FdtAddress & 7)) {
    mBootData.FdtStatus = EFI_INVALID_PARAMETER;
    return;
  }
  Status = MemoryMap (&Map, &Count, &Heap);
  if (EFI_ERROR (Status)) { mBootData.FdtStatus = Status; return; }
#ifdef CR_BOOT_DATA_SMEM
  CaptureSmemRam (Map, Count);
#endif
  if (!Readable (Map, Count, FdtAddress, sizeof (Header))) return;
  CopyMem (Header, (CONST VOID *)FdtAddress, sizeof (Header));
  Size = Be32 ((CONST UINT8 *)Header + 4);
  DEBUG ((DEBUG_ERROR, "BootDataCr: FDT header magic=0x%x bytes=0x%x version=%u\n",
          Be32 ((CONST UINT8 *)Header), Size, Be32 ((CONST UINT8 *)Header + 20)));
  if (Be32 ((CONST UINT8 *)Header) != 0xd00dfeed || Size < sizeof (Header) ||
      Size > CR_BOOT_DATA_FDT_MAX || Be32 ((CONST UINT8 *)Header + 20) != 17) {
    mBootData.FdtStatus = EFI_COMPROMISED_DATA;
    return;
  }
  if (!Readable (Map, Count, FdtAddress, Size)) return;
  Status = InitrdTail ((CONST UINT8 *)FdtAddress, Size, &Tail, &TailSize);
  if (!EFI_ERROR (Status) && TailSize != 0 &&
      (!Readable (Map, Count, Tail, TailSize) || Intersects (FdtAddress, Size, Tail, TailSize)))
    Status = EFI_COMPROMISED_DATA;
  if (EFI_ERROR (Status)) Tail = TailSize = 0;
  mBootData.TailStatus = Status;
  TailOffset = ((UINT64)Size + 7) & ~(UINT64)7;
  AllocationSize = (TailOffset + TailSize + EFI_PAGE_MASK) & ~(UINT64)EFI_PAGE_MASK;
  Status = Arena (Map, Count, Heap, HeapBase, *HeapFreeTop, FdtAddress, Size,
                  Tail, TailSize, AllocationSize, &Allocation);
  if (EFI_ERROR (Status)) { mBootData.FdtStatus = Status; return; }
  if (CompareMem (Header, (CONST VOID *)FdtAddress, sizeof (Header)) != 0) {
    mBootData.FdtStatus = EFI_COMPROMISED_DATA;
    return;
  }
  /* Both original spans and their disjoint destination have been established
   * before the first write. Never replace this with two independent memmoves. */
  CopyMem ((VOID *)(UINTN)Allocation, (CONST VOID *)FdtAddress, Size);
  if (TailSize != 0)
    CopyMem ((VOID *)(UINTN)(Allocation + TailOffset), (CONST VOID *)(UINTN)Tail, (UINTN)TailSize);
  *HeapFreeTop = (UINTN)Allocation;
  mBootData.AllocationBase = Allocation;
  mBootData.AllocationSize = AllocationSize;
  mBootData.FdtCopy = Allocation;
  mBootData.FdtSize = Size;
  mBootData.TailOriginal = Tail;
  mBootData.TailCopy = TailSize != 0 ? Allocation + TailOffset : 0;
  mBootData.TailSize = TailSize;
  mBootData.FdtStatus = EFI_SUCCESS;
}

VOID EFIAPI BootModePublish (VOID)
{
  EFI_HOB_MEMORY_ALLOCATION *Allocation;
  if (mPublished) return;
  mPublished = TRUE;
  if (!EFI_ERROR (mBootData.FdtStatus)) {
    Allocation = CreateHob (EFI_HOB_TYPE_MEMORY_ALLOCATION, sizeof (*Allocation));
    if (Allocation == NULL) {
      DEBUG ((DEBUG_ERROR, "BootDataCr: allocation HOB failed\n"));
      return;
    }
    ZeroMem (&Allocation->AllocDescriptor, sizeof (Allocation->AllocDescriptor));
    CopyGuid (&Allocation->AllocDescriptor.Name, &gCrBootDataGuid);
    Allocation->AllocDescriptor.MemoryBaseAddress = mBootData.AllocationBase;
    Allocation->AllocDescriptor.MemoryLength = mBootData.AllocationSize;
    Allocation->AllocDescriptor.MemoryType = EfiBootServicesData;
  }
  if (BuildGuidDataHob (&gCrBootDataGuid, &mBootData, sizeof (mBootData)) == NULL)
    DEBUG ((DEBUG_ERROR, "BootDataCr: descriptor HOB failed\n"));
  DEBUG ((DEBUG_ERROR, "BootDataCr: raw FDT=%r bytes=0x%Lx tail=%r bytes=0x%Lx\n",
          mBootData.FdtStatus, mBootData.FdtSize, mBootData.TailStatus, mBootData.TailSize));
}
