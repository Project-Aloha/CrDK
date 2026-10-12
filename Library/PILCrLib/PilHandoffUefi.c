/* Final, immutable HDK8450 PIL ownership transfer. SPDX-License-Identifier: MIT */
#include <PiDxe.h>
#include <Library/PILCrHandoffUefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>

#define PIL_MAP_MIN_CAPACITY 65536U
#define PIL_MAP_MAX_CAPACITY (1024U * 1024U)

STATIC BOOLEAN Covers(UINT64 Base, UINT64 Size, UINT64 Start, UINT64 Length)
{
  return Size != 0 && Base <= MAX_UINT64 - Size && Length != 0 &&
         Start >= Base && Length <= Size && Start - Base <= Size - Length;
}

/* Only normal RAM may hold the ACPI-described record; Device/UC storage is
   not suitable for ordinary structure accesses or cached Windows mappings. */
STATIC EFI_STATUS NormalRam(UINT64 Base, UINT64 Size, BOOLEAN Carveout)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS Status = gDS->GetMemorySpaceDescriptor(Base, &D);
  if (EFI_ERROR(Status)) return Status;
  if (!Covers(D.BaseAddress, D.Length, Base, Size) ||
      (D.Attributes & (EFI_MEMORY_RP | EFI_MEMORY_WP | EFI_MEMORY_RO)) != 0 ||
      (!Carveout && ((D.Attributes & (EFI_MEMORY_UC | EFI_MEMORY_UCE)) != 0 ||
       (D.Attributes & (EFI_MEMORY_WB | EFI_MEMORY_WT | EFI_MEMORY_WC)) == 0)) ||
      (Carveout ? D.GcdMemoryType != EfiGcdMemoryTypeReserved :
       D.GcdMemoryType != EfiGcdMemoryTypeSystemMemory && D.GcdMemoryType != EfiGcdMemoryTypeReserved))
    return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}

EFI_STATUS CrPilHandoffPrepare(CR_PIL_UEFI_HANDOFF *S)
{
  EFI_STATUS Status;
  UINTN Size, Key, Stride;
  UINT32 Version;
  CR_BUS_PIL_LOCATOR Table = {0};
  if (S == NULL) return EFI_INVALID_PARAMETER;
  if (S->Installed) return EFI_SUCCESS;
  if (S->Published) return EFI_ACCESS_DENIED;
  if (S->Acpi == NULL) {
    Status = gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid, NULL, (VOID **)&S->Acpi);
    if (EFI_ERROR(Status)) return Status;
    if (S->Acpi == NULL || S->Acpi->InstallAcpiTable == NULL || S->Acpi->UninstallAcpiTable == NULL) {
      S->Acpi = NULL; return EFI_UNSUPPORTED;
    }
  }
  if (S->Generation == 0) {
    /* EmbeddedMonotonicCounter returns zero on its first successful call.
       Zero is our unpublished sentinel, so consume that value and request
       the next count now: ReadyToBoot may be the final preparation chance. */
    for (UINTN Attempt = 0; Attempt < 2 && S->Generation == 0; ++Attempt) {
      Status = gBS->GetNextMonotonicCount(&S->Generation);
      if (EFI_ERROR(Status)) {S->Generation = 0; return Status;}
    }
    if (S->Generation == 0) return EFI_NOT_READY;
  }
  if (!S->RecordOwned) {
    Status = gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, 1, &S->Record);
    if (EFI_ERROR(Status)) {S->Record = 0; return Status;}
    S->RecordOwned = TRUE;
  }
  if (S->Record < 0x100000 || (S->Record & EFI_PAGE_MASK) != 0) return EFI_UNSUPPORTED;
  Status = NormalRam(S->Record, EFI_PAGE_SIZE, FALSE);
  if (EFI_ERROR(Status)) return Status;
  ZeroMem((VOID *)(UINTN)S->Record, EFI_PAGE_SIZE);
  WriteBackDataCacheRange((VOID *)(UINTN)S->Record, EFI_PAGE_SIZE);
  if (S->Map == NULL) {
    Size = 0; Stride = 0;
    Status = gBS->GetMemoryMap(&Size, NULL, &Key, &Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) return EFI_ERROR(Status) ? Status : EFI_COMPROMISED_DATA;
    if (Stride < sizeof(EFI_MEMORY_DESCRIPTOR) || Stride > 1024 ||
        Size > PIL_MAP_MAX_CAPACITY - 128 * Stride) return EFI_OUT_OF_RESOURCES;
    Size += 128 * Stride;
    if (Size < PIL_MAP_MIN_CAPACITY) Size = PIL_MAP_MIN_CAPACITY;
    Status = gBS->AllocatePool(EfiBootServicesData, Size, (VOID **)&S->Map);
    if (EFI_ERROR(Status)) {S->Map = NULL; return Status;}
    S->MapCapacity = Size;
  }
  Table.Signature = CR_BUS_PIL_LOCATOR_SIGNATURE; Table.Length = sizeof(Table);
  Table.Revision = 1; CopyMem(Table.OemId, "ALOHA ", 6); CopyMem(Table.OemTableId, "PILHDK84", 8);
  Table.OemRevision = 1; Table.CreatorId = SIGNATURE_32('C','R','D','K'); Table.CreatorRevision = 1;
  Table.RecordPhysical = S->Record; Table.RecordLength = CR_BUS_PIL_HANDOFF_SIZE;
  Table.RecordGeneration = S->Generation;
  Table.Checksum = CalculateCheckSum8((UINT8 *)&Table, sizeof(Table));
  Status = S->Acpi->InstallAcpiTable(S->Acpi, &Table, sizeof(Table), &S->TableKey);
  if (EFI_ERROR(Status)) return Status;
  S->Installed = TRUE;
  return EFI_SUCCESS;
}

EFI_STATUS CrPilHandoffRelease(CR_PIL_UEFI_HANDOFF *S)
{
  EFI_STATUS Status;
  if (S == NULL) return EFI_INVALID_PARAMETER;
  if (S->Published) return EFI_ACCESS_DENIED;
  if (S->Installed) {
    Status = S->Acpi->UninstallAcpiTable(S->Acpi, S->TableKey);
    if (EFI_ERROR(Status)) return Status;
    S->Installed = FALSE; S->TableKey = 0;
  }
  if (S->Map != NULL) {
    Status = gBS->FreePool(S->Map);
    if (EFI_ERROR(Status)) return Status;
    S->Map = NULL; S->MapCapacity = 0;
  }
  if (S->RecordOwned) {
    Status = gBS->FreePages(S->Record, 1);
    if (EFI_ERROR(Status)) return Status;
    S->Record = 0; S->RecordOwned = FALSE;
  }
  S->Generation = 0; S->Acpi = NULL;
  return EFI_SUCCESS;
}

/* GetMemoryMap is read-only in this platform's DXE core, including after
   CoreTerminateMemoryMap. Caller storage was allocated before EBS. */
STATIC EFI_STATUS FinalMap(CR_PIL_UEFI_HANDOFF *S, CR_BUS_PIL_HANDOFF *H)
{
  UINTN Size = S->MapCapacity, Key, Stride;
  UINT32 Version;
  BOOLEAN HaveRecord = FALSE, HaveCarveout = FALSE;
  EFI_STATUS Status = gBS->GetMemoryMap(&Size, S->Map, &Key, &Stride, &Version);
  if (EFI_ERROR(Status)) return Status;
  if (Version != EFI_MEMORY_DESCRIPTOR_VERSION || Stride < sizeof(EFI_MEMORY_DESCRIPTOR) ||
      (Stride & 7) != 0 || Size > S->MapCapacity || Size % Stride != 0) return EFI_COMPROMISED_DATA;
  for (UINTN Offset = 0; Offset < Size; Offset += Stride) {
    EFI_MEMORY_DESCRIPTOR D;
    CopyMem(&D, (UINT8 *)S->Map + Offset, sizeof(D));
    if (D.NumberOfPages == 0 || D.NumberOfPages > MAX_UINT64 / EFI_PAGE_SIZE ||
        (D.PhysicalStart & EFI_PAGE_MASK) != 0) return EFI_COMPROMISED_DATA;
    UINT64 Length = D.NumberOfPages * EFI_PAGE_SIZE;
    if (D.PhysicalStart > MAX_UINT64 - Length) return EFI_COMPROMISED_DATA;
    if (D.PhysicalStart < S->Record + EFI_PAGE_SIZE && S->Record < D.PhysicalStart + Length) {
      if (!Covers(D.PhysicalStart, Length, S->Record, EFI_PAGE_SIZE)) return EFI_ACCESS_DENIED;
      if (HaveRecord || D.Type != EfiReservedMemoryType) return EFI_ACCESS_DENIED;
      HaveRecord = TRUE;
    }
    if (D.PhysicalStart < H->CarveoutBase + H->CarveoutSize && H->CarveoutBase < D.PhysicalStart + Length) {
      if (!Covers(D.PhysicalStart, Length, H->CarveoutBase, H->CarveoutSize)) return EFI_ACCESS_DENIED;
      if (HaveCarveout || D.Type != EfiReservedMemoryType ||
          (D.Attribute & (EFI_MEMORY_RP | EFI_MEMORY_WP | EFI_MEMORY_RO)) != 0) return EFI_ACCESS_DENIED;
      HaveCarveout = TRUE; H->MemoryDescriptorBase = D.PhysicalStart;
      H->MemoryDescriptorSize = Length; H->MemoryDescriptorType = D.Type;
      H->MemoryDescriptorAttributes = D.Attribute;
    }
  }
  return HaveRecord && HaveCarveout ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS CrPilHandoffPublish(CR_PIL_UEFI_HANDOFF *S, CONST CR_PIL_CONTEXT *Pil,
                               CONST CR_PIL_CLIENTS *Clients, EFI_RPMH_CR_PROTOCOL *Rpmh)
{
  CR_BUS_PIL_HANDOFF H = {0};
  EFI_RPMH_CR_ARC_SNAPSHOT Arc = {0};
  EFI_STATUS Status;
  UINT32 Count = 0, Crc, Offset, MailboxSize;
  CONST CHAR8 *Names[6] = {"cx.lvl", "mx.lvl", "lcx.lvl", "lmx.lvl", "mmcx.lvl", "xo.lvl"};
  if (S == NULL || Pil == NULL || Clients == NULL || Rpmh == NULL || Pil->Target == NULL)
    return EFI_INVALID_PARAMETER;
  if (S->Published) return EFI_SUCCESS;
  if (!S->Installed || !S->RecordOwned || S->Record == 0 || S->Map == NULL || S->Generation == 0 ||
      !Clients->Frozen || Clients->Dispatching || Pil->PasOwned || Pil->Running || Pil->Votes != 0 ||
      Pil->CryptoPath != 0 || Pil->LoadState || Pil->SmcInterrupted || Pil->SmcCookie != 0 ||
      Pil->QmpOpen || Pil->QmpChanged || Pil->QmpClosing || Pil->QmpEstablished || !Pil->Shutdown.Complete ||
      !Pil->Shutdown.Attempted || !Pil->Shutdown.Returned || Pil->Shutdown.CallStatus != CR_SUCCESS ||
      Pil->Shutdown.X0 != 0 || Pil->Shutdown.X1 != 0 || Pil->StartGeneration == 0 ||
      Pil->Shutdown.Generation != Pil->StartGeneration ||
      Pil->NativeShutdownGeneration != Pil->StartGeneration ||
      (Pil->NativeShutdownEl != 1 && Pil->NativeShutdownEl != 2)) return EFI_NOT_READY;
  if ((Pil->SmcBase != 0x42000000U && Pil->SmcBase != 0x02000000U) ||
      Pil->Shutdown.FunctionId != (Pil->SmcBase | 0x206U) ||
      Pil->Target->PasId != 1 || Pil->Target->MemoryBase != 0x85e00000U ||
      Pil->Target->MemorySize != 0x02100000U || Pil->Target->QmpBase != 0x0c300000U ||
      Pil->Target->QmpSize != 0x400 || Pil->Target->IpccSend != 0x0ed1800cU ||
      Pil->Target->IpccClient != 3 || Pil->Io.Read32 == NULL || Pil->Io.Fence == NULL ||
      Rpmh->Revision < EFI_RPMH_CR_PROTOCOL_REVISION || Rpmh->SealArcSnapshot == NULL)
    return EFI_UNSUPPORTED;
  for (UINT32 I = 0; I < CR_PIL_MAX_CLIENTS; ++I) if (Clients->Entries[I].Owner != NULL) {
    if (!Clients->Entries[I].Quiet || Clients->Entries[I].Quiesce == NULL) return EFI_NOT_READY;
    ++Count;
  }
  if (Count != Clients->Count) return EFI_COMPROMISED_DATA;
  /* Registered UCSI owns every other active HDK IPCC writer/receive IRQ.
     Its synchronous quiesce and the successful owned PAS shutdown retire
     those transports. PIL owns no IRQ. The fence completes local writes;
     this field does not claim a hardware SEND_ID acknowledgement register. */
  Pil->Io.Fence(Pil->Io.Context);
  UINT32 Base = Pil->Target->QmpBase;
  if (Pil->Io.Read32(Pil->Io.Context, Base) != 0x4d41494cU ||
      Pil->Io.Read32(Pil->Io.Context, Base + 4) != 1) return EFI_COMPROMISED_DATA;
  MailboxSize = Pil->Io.Read32(Pil->Io.Context, Base + 0x34);
  Offset = Pil->Io.Read32(Pil->Io.Context, Base + 0x38);
  if ((Offset & 3) != 0 || Offset < 0x3c || Offset > Pil->Target->QmpSize ||
      MailboxSize < 68 || MailboxSize > Pil->Target->QmpSize - Offset || Offset != Pil->QmpOffset)
    return EFI_COMPROMISED_DATA;
  H.QmpPendingBytes = Pil->Io.Read32(Pil->Io.Context, Base + Offset);
  H.QmpLocalLink = Pil->Io.Read32(Pil->Io.Context, Base + 0x24);
  H.QmpRemoteLink = Pil->Io.Read32(Pil->Io.Context, Base + 0x28);
  H.QmpLocalChannel = Pil->Io.Read32(Pil->Io.Context, Base + 0x2c);
  H.QmpRemoteChannel = Pil->Io.Read32(Pil->Io.Context, Base + 0x30);
  if (H.QmpPendingBytes != 0 || H.QmpLocalLink != H.QmpRemoteLink ||
      H.QmpLocalChannel != H.QmpRemoteChannel ||
      (H.QmpLocalLink != 0xffffU && H.QmpLocalLink != 0xffff0000U) ||
      (H.QmpLocalChannel != 0xffffU && H.QmpLocalChannel != 0xffff0000U)) return EFI_NOT_READY;
  H.CarveoutBase = Pil->Target->MemoryBase; H.CarveoutSize = Pil->Target->MemorySize;
  Status = NormalRam(H.CarveoutBase, H.CarveoutSize, TRUE);
  if (EFI_ERROR(Status)) return Status;
  Status = NormalRam(S->Record, EFI_PAGE_SIZE, FALSE);
  if (EFI_ERROR(Status)) return Status;
  Status = FinalMap(S, &H);
  if (EFI_ERROR(Status)) return Status;
  Status = Rpmh->SealArcSnapshot(Rpmh, &Arc);
  if (Status != EFI_SUCCESS) return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
  if (Arc.Size != sizeof(Arc) || Arc.Revision != EFI_RPMH_CR_ARC_SNAPSHOT_REVISION ||
      Arc.Count != 6 || Arc.OwnerCount != 1 || Arc.Flags !=
      (EFI_RPMH_CR_ARC_FLOOR_ACKNOWLEDGED | EFI_RPMH_CR_ARC_CONSUMERS_UNSYNCHRONIZED | EFI_RPMH_CR_ARC_REGISTRY_SEALED))
    return EFI_COMPROMISED_DATA;
  for (UINT32 I = 0; I < 6; ++I) {
    CONST CR_RPMH_ARC_RESOURCE *R = &Arc.Resources[I];
    UINTN N = AsciiStrLen(Names[I]);
    if (CompareMem(R->Name, Names[I], N + 1) != 0 || R->LevelCount > 32 ||
        R->Address == 0 || R->Kind != (I == 5 ? CrRpmhArcXo : CrRpmhArcPower) ||
        (I != 5 && R->LevelCount == 0) || Arc.Corners[I] != (I == 5 ? 3 : R->LevelCount - 1))
      return EFI_COMPROMISED_DATA;
    UINT8 Aux[64];
    CR_RPMH_ARC_RESOURCE Qualified;
    for (UINT32 L = 0; L < R->LevelCount; ++L) {
      Aux[2 * L] = (UINT8)R->Levels[L]; Aux[2 * L + 1] = (UINT8)(R->Levels[L] >> 8);
    }
    Status = CrRpmhArcQualify(&Qualified, Names[I], R->Address, R->Kind, Aux, 2 * R->LevelCount);
    if (Status != EFI_SUCCESS || Qualified.LevelCount != R->LevelCount) return EFI_COMPROMISED_DATA;
    for (UINT32 P = 0; P < I; ++P) if (H.Arc[P].Address == R->Address) return EFI_COMPROMISED_DATA;
    CopyMem(H.Arc[I].Name, Names[I], N); H.Arc[I].Address = R->Address;
    H.Arc[I].Corner = Arc.Corners[I]; H.Arc[I].LevelCount = R->LevelCount;
    CopyMem(H.Arc[I].Levels, R->Levels, R->LevelCount * sizeof(UINT16));
    H.Arc[I].Provenance = CR_BUS_PIL_ARC_UNSYNCHRONIZED_BOOT_FLOOR;
  }
  H.Signature = CR_BUS_PIL_HANDOFF_SIGNATURE; H.Revision = CR_BUS_PIL_HANDOFF_REVISION;
  H.Size = sizeof(H); H.Platform = CR_BUS_PIL_HANDOFF_PLATFORM;
  H.Generation = S->Generation; H.PublishSequence = 2; H.Phase = CR_BUS_PIL_HANDOFF_FINAL_EBS;
  H.PasDisposition = CR_BUS_PIL_PAS_OWNED_SHUTDOWN; H.FirmwareEl = Pil->NativeShutdownEl;
  H.NativeConduit = CR_BUS_PIL_NATIVE_CONDUIT_SMC; H.NativeConvention = Pil->SmcBase;
  H.QmpBase = Base; H.QmpSize = Pil->Target->QmpSize; H.IpccSend = Pil->Target->IpccSend;
  H.ArcCount = 6;
  Crc = CalculateCrc32(&H, sizeof(H) - sizeof(H.Checksum));
  H.Checksum = Crc;
  volatile CR_BUS_PIL_HANDOFF *Wire = (VOID *)(UINTN)S->Record;
  Wire->PublishSequence = 1; MemoryFence(); H.PublishSequence = 1;
  CopyMem((VOID *)Wire, &H, sizeof(H));
  WriteBackDataCacheRange((VOID *)Wire, sizeof(H)); MemoryFence();
  Wire->PublishSequence = 2;
  WriteBackDataCacheRange((VOID *)&Wire->PublishSequence, sizeof(Wire->PublishSequence));
  MemoryFence(); S->Published = TRUE;
  return EFI_SUCCESS;
}
