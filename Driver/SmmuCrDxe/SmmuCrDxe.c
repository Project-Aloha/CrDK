/** @file
  Crane SMMUv2/MMU-500 adapter for the EDKII IOMMU protocol.

  The Qualcomm HALIOMMU binary may have configured firmware clients before this
  driver is dispatched.  Without an explicit target handoff proving that the
  HAL owns the PCIe streams, this driver refuses to claim the controller and
  leaves PCIe cold initialization unavailable.  When no external producer is
  present, it probes and adopts the controller in place, allocates only unused
  context banks/stream-match entries, and never resets global SMMU state.

  SPDX-License-Identifier: MIT
**/

#include <PiDxe.h>

#include <Guid/EventGroup.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/IoMmu.h>

#include <Library/CrTargetSmmuLib.h>
#include <Library/smmu.h>
#include <Protocol/EFISmmuCrProtocol.h>

#define CR_SMMU_MAX_DOMAINS       32U
#define CR_SMMU_MAX_MAPPINGS      128U
#define CR_SMMU_MAPPING_SIGNATURE 0x554D4D53U
#define CR_SMMU_BDF_UNKNOWN       0xFFFFU

typedef struct {
  BOOLEAN             InUse;
  BOOLEAN             Poisoned;
  UINT16              Segment;
  UINT16              Bdf;
  UINT16              Sid;
  VOID               *Tables;
  EFI_PHYSICAL_ADDRESS TableAddress;
  SmmuDomain          Domain;
} CR_SMMU_DOMAIN;

typedef struct {
  UINT32                    Signature;
  BOOLEAN                   InUse;
  EDKII_IOMMU_OPERATION     Operation;
  EFI_PHYSICAL_ADDRESS      HostAddress;
  EFI_PHYSICAL_ADDRESS      PhysicalBase;
  EFI_PHYSICAL_ADDRESS      IovaBase;
  UINTN                     NumberOfBytes;
  UINTN                     Pages;
  UINT32                    Access;
  /* SetAttributeById may target one SID while the generic entry point
     updates every attached SID.  Keep the per-domain state explicit. */
  UINT32                    DomainAccess[CR_SMMU_MAX_DOMAINS];
} CR_SMMU_MAPPING;

typedef struct {
  SmmuDevice                Device;
  CONST CrTargetSmmuContext *Target;
  CR_SMMU_DOMAIN            Domains[CR_SMMU_MAX_DOMAINS];
  CR_SMMU_MAPPING           Mappings[CR_SMMU_MAX_MAPPINGS];
  UINT8                     IovaBitmap[SMMU_IOVA_SIZE / SMMU_PAGE_SIZE / 8U];
  UINTN                     DomainLimit;
  EFI_LOCK                  Lock;
  BOOLEAN                   Initialized;
  BOOLEAN                   AfterExitBootServices;
  BOOLEAN                   ExistingIoMmu;
  EFI_EVENT                 ExitBootServicesEvent;
} CR_SMMU_STATE;

STATIC CR_SMMU_STATE          mSmmu;
STATIC EFI_HANDLE             mSmmuImageHandle;
STATIC EDKII_IOMMU_PROTOCOL   mIoMmuProtocol;
STATIC EFI_SMMU_CR_PROTOCOL   mCrProtocol;

STATIC
BOOLEAN
AddressRangeFitsDevice (
  IN EFI_PHYSICAL_ADDRESS Address,
  IN UINTN                Pages
  )
{
  UINT64 Limit;
  UINT64 Bytes;

  if (Pages == 0 || Pages > MAX_UINT64 / SMMU_PAGE_SIZE) {
    return FALSE;
  }
  Bytes = (UINT64)Pages * SMMU_PAGE_SIZE;
  if (mSmmu.Device.AddressBits >= 64) {
    Limit = MAX_UINT64;
  } else if (mSmmu.Device.AddressBits == 0) {
    return FALSE;
  } else {
    Limit = (1ULL << mSmmu.Device.AddressBits) - 1ULL;
  }
  return (Address <= Limit) && ((Bytes - 1ULL) <= (Limit - Address));
}

STATIC
UINT32
EFIAPI
CrSmmuRead32 (
  IN VOID   *Cookie,
  IN UINT64  Address
  )
{
  (VOID)Cookie;
  return MmioRead32 ((UINTN)Address);
}

STATIC
VOID
EFIAPI
CrSmmuWrite32 (
  IN VOID   *Cookie,
  IN UINT64  Address,
  IN UINT32  Value
  )
{
  (VOID)Cookie;
  MmioWrite32 ((UINTN)Address, Value);
}

STATIC
VOID
EFIAPI
CrSmmuWrite64 (
  IN VOID   *Cookie,
  IN UINT64  Address,
  IN UINT64  Value
  )
{
  (VOID)Cookie;
  MmioWrite64 ((UINTN)Address, Value);
}

STATIC
VOID
EFIAPI
CrSmmuPublish (
  IN VOID   *Cookie,
  IN VOID   *Address,
  IN UINTN   Size
  )
{
  (VOID)Cookie;
  WriteBackDataCacheRange (Address, Size);
}

STATIC
VOID
EFIAPI
CrSmmuDelayUs (
  IN VOID   *Cookie,
  IN UINT32  Delay
  )
{
  (VOID)Cookie;
  MicroSecondDelay (Delay);
}

STATIC
CONST SmmuIoOps mSmmuIoOps = {
  CrSmmuRead32,
  CrSmmuWrite32,
  CrSmmuWrite64,
  CrSmmuPublish,
  CrSmmuDelayUs,
  NULL
};

STATIC
BOOLEAN
SidAllowed (
  IN UINT16 Sid
  )
{
  UINT16 Index;

  if ((mSmmu.Target == NULL) ||
      (mSmmu.Target->DefaultStreamCount == 0) ||
      (mSmmu.Target->DefaultStreamIds == NULL)) {
    return TRUE;
  }

  for (Index = 0; Index < mSmmu.Target->DefaultStreamCount; Index++) {
    if (mSmmu.Target->DefaultStreamIds[Index] == Sid) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
CR_SMMU_DOMAIN *
FindDomainBySid (
  IN UINT16 Sid
  )
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (mSmmu.Domains[Index].InUse &&
        (mSmmu.Domains[Index].Sid == Sid)) {
      return &mSmmu.Domains[Index];
    }
  }

  return NULL;
}

STATIC
BOOLEAN
AnyDomainInUse (
  VOID
  )
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (mSmmu.Domains[Index].InUse) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
FindDomainIndex (
  IN  CR_SMMU_DOMAIN *Domain,
  OUT UINTN           *Index
  )
{
  UINTN Candidate;

  if ((Domain == NULL) || (Index == NULL)) {
    return FALSE;
  }
  for (Candidate = 0; Candidate < ARRAY_SIZE (mSmmu.Domains); Candidate++) {
    if (Domain == &mSmmu.Domains[Candidate]) {
      *Index = Candidate;
      return TRUE;
    }
  }
  return FALSE;
}

STATIC
BOOLEAN
AnyMappingInUse (
  VOID
  )
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    if (mSmmu.Mappings[Index].InUse) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
BOOLEAN
BitmapIsSet (
  IN UINTN Page
  )
{
  return (mSmmu.IovaBitmap[Page / 8U] & (UINT8)(1U << (Page % 8U))) != 0;
}

STATIC
VOID
BitmapSet (
  IN UINTN Page
  )
{
  mSmmu.IovaBitmap[Page / 8U] |= (UINT8)(1U << (Page % 8U));
}

STATIC
VOID
BitmapClear (
  IN UINTN Page
  )
{
  mSmmu.IovaBitmap[Page / 8U] &= (UINT8)~(1U << (Page % 8U));
}

STATIC
BOOLEAN
FindIovaRange (
  IN  UINTN  Pages,
  OUT UINTN  *FirstPage
  )
{
  UINTN Candidate;
  UINTN Run;
  UINTN PageCount;

  if ((Pages == 0) || (FirstPage == NULL)) {
    return FALSE;
  }

  PageCount = SMMU_IOVA_SIZE / SMMU_PAGE_SIZE;
  if (Pages > PageCount) {
    return FALSE;
  }

  for (Candidate = 0; Candidate <= PageCount - Pages; Candidate++) {
    for (Run = 0; Run < Pages; Run++) {
      if (BitmapIsSet (Candidate + Run)) {
        break;
      }
    }
    if (Run == Pages) {
      *FirstPage = Candidate;
      for (Run = 0; Run < Pages; Run++) {
        BitmapSet (Candidate + Run);
      }
      return TRUE;
    }
    Candidate += Run;
  }

  return FALSE;
}

STATIC
VOID
ReleaseIovaRange (
  IN EFI_PHYSICAL_ADDRESS IovaBase,
  IN UINTN                Pages
  )
{
  UINTN FirstPage;
  UINTN Index;

  FirstPage = (UINTN)((IovaBase - SMMU_IOVA_BASE) / SMMU_PAGE_SIZE);
  for (Index = 0; Index < Pages; Index++) {
    BitmapClear (FirstPage + Index);
  }
}

STATIC
CR_SMMU_MAPPING *
FindMapping (
  IN VOID *Mapping
  )
{
  UINTN Index;

  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    if (mSmmu.Mappings[Index].InUse &&
        (mSmmu.Mappings[Index].Signature == CR_SMMU_MAPPING_SIGNATURE) &&
        (Mapping == &mSmmu.Mappings[Index])) {
      return &mSmmu.Mappings[Index];
    }
  }

  return NULL;
}

STATIC
EFI_STATUS
AllocateDomainTables (
  IN OUT CR_SMMU_DOMAIN *Slot
  )
{
  EFI_PHYSICAL_ADDRESS Address;
  EFI_PHYSICAL_ADDRESS Maximum;
  EFI_STATUS            Status;

  if ((Slot == NULL) || (gBS == NULL) || (gBS->AllocatePages == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if (mSmmu.Device.AddressBits >= 64) {
    Maximum = MAX_UINT64;
  } else if (mSmmu.Device.AddressBits == 0) {
    return EFI_UNSUPPORTED;
  } else {
    Maximum = (1ULL << mSmmu.Device.AddressBits) - 1ULL;
  }
  Address = Maximum;
  Status  = gBS->AllocatePages (
                         AllocateMaxAddress,
                         EfiBootServicesData,
                         SMMU_TABLE_PAGES,
                         &Address
                         );
  if (EFI_ERROR (Status)) {
    return Status;
  }
  if (((Address & (SMMU_PAGE_SIZE - 1U)) != 0) ||
      !AddressRangeFitsDevice (Address, SMMU_TABLE_PAGES)) {
    gBS->FreePages (Address, SMMU_TABLE_PAGES);
    return EFI_OUT_OF_RESOURCES;
  }

  Slot->TableAddress = Address;
  Slot->Tables       = (VOID *)(UINTN)Address;
  ZeroMem (Slot->Tables, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE);
  return EFI_SUCCESS;
}

STATIC
VOID
FreeDomainTables (
  IN OUT CR_SMMU_DOMAIN *Slot
  )
{
  if ((Slot != NULL) && (Slot->Tables != NULL) && (gBS != NULL) &&
      (gBS->FreePages != NULL)) {
    gBS->FreePages (Slot->TableAddress, SMMU_TABLE_PAGES);
  }
  if (Slot != NULL) {
    Slot->Tables       = NULL;
    Slot->TableAddress = 0;
  }
}

STATIC
EFI_STATUS
AttachStreamLocked (
  IN UINT16 Segment,
  IN UINT16 Bdf,
  IN UINT16 Sid
  )
{
  CR_SMMU_DOMAIN *Existing;
  CR_SMMU_DOMAIN *Slot;
  EFI_STATUS      Status;
  UINTN           Index;

  if (!SidAllowed (Sid)) {
    return EFI_UNSUPPORTED;
  }
  /* A newly attached SID would otherwise miss mappings already installed on
     the existing domains.  Cold PCIe attach is required before any DMA map. */
  if (AnyMappingInUse ()) {
    return EFI_ACCESS_DENIED;
  }
  Existing = FindDomainBySid (Sid);
  if (Existing != NULL) {
    /* A BDF can legitimately emit two calls for one SID during enumeration. */
    if ((Existing->Segment == Segment) || (Bdf == CR_SMMU_BDF_UNKNOWN) ||
        (Existing->Bdf == CR_SMMU_BDF_UNKNOWN)) {
      if (Existing->Bdf == CR_SMMU_BDF_UNKNOWN) {
        Existing->Bdf = Bdf;
      }
      return EFI_SUCCESS;
    }
    return EFI_ACCESS_DENIED;
  }

  Slot = NULL;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (!mSmmu.Domains[Index].InUse && !mSmmu.Domains[Index].Poisoned) {
      Slot = &mSmmu.Domains[Index];
      break;
    }
  }
  if ((Slot == NULL) || (Index >= mSmmu.DomainLimit)) {
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Slot, sizeof (*Slot));
  Status = AllocateDomainTables (Slot);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = (EFI_STATUS)SmmuAttach (
                          &mSmmu.Device,
                          &Slot->Domain,
                          Sid,
                          Slot->Tables,
                          Slot->TableAddress
                          );
  if (EFI_ERROR (Status)) {
    if (Slot->Domain.Failed) {
      /* SmmuAttach may have performed irreversible MMIO writes. */
      Slot->Poisoned = TRUE;
    } else {
      FreeDomainTables (Slot);
      ZeroMem (Slot, sizeof (*Slot));
    }
    return Status;
  }

  Slot->InUse   = TRUE;
  Slot->Segment = Segment;
  Slot->Bdf     = Bdf;
  Slot->Sid     = Sid;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuAttach (
  IN EFI_SMMU_CR_PROTOCOL *This,
  IN UINT16                Segment,
  IN UINT16                Bdf,
  IN UINT16                Sid
  )
{
  EFI_STATUS Status;

  if ((This != &mCrProtocol) || !mSmmu.Initialized ||
      mSmmu.AfterExitBootServices || (mSmmu.Target == NULL) ||
      (Segment >= mSmmu.Target->SegmentCount) || (Sid > 0x7FFFU)) {
    return EFI_INVALID_PARAMETER;
  }

  if (!SidAllowed (Sid)) {
    return EFI_UNSUPPORTED;
  }
  /* HALIOMMU is the owner in this mode.  Its already-installed stream
     configuration is deliberately left untouched only when the target has
     explicitly documented that ownership hand-off.  Merely finding an
     EDKII_IOMMU_PROTOCOL does not establish that the PCIe SIDs were set up;
     in that case this adapter owns only the still-unused stream/context
     resources and leaves every active firmware resource alone. */
  if (mSmmu.ExistingIoMmu) {
    if (mSmmu.Target->ExternalIoMmuOwnsPcieStreams) {
      return EFI_SUCCESS;
    }
  }

  EfiAcquireLock (&mSmmu.Lock);
  Status = AttachStreamLocked (Segment, Bdf, Sid);
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuBlockSegment (
  IN EFI_SMMU_CR_PROTOCOL *This,
  IN UINT16                Segment
  )
{
  EFI_STATUS      Status;
  EFI_STATUS      FirstError;
  UINTN           Index;
  CR_SMMU_DOMAIN *Slot;

  if ((This != &mCrProtocol) || !mSmmu.Initialized ||
      mSmmu.AfterExitBootServices || (mSmmu.Target == NULL) ||
      (Segment >= mSmmu.Target->SegmentCount)) {
    return EFI_INVALID_PARAMETER;
  }

  /* An existing HALIOMMU owns its stream/context state only when the target
     explicitly says that it owns the PCIe streams.  Otherwise the domains
     below contain only resources attached by this adapter; SmmuBlock and
     SmmuDetach restore those resources without touching active firmware
     streams or context banks. */
  if (mSmmu.ExistingIoMmu) {
    if (mSmmu.Target->ExternalIoMmuOwnsPcieStreams) {
      return EFI_SUCCESS;
    }
  }

  EfiAcquireLock (&mSmmu.Lock);
  if (AnyMappingInUse ()) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_ACCESS_DENIED;
  }

  FirstError = EFI_NOT_FOUND;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    Slot = &mSmmu.Domains[Index];
    if (!Slot->InUse || (Slot->Segment != Segment)) {
      continue;
    }
    FirstError = EFI_SUCCESS;
    Status     = (EFI_STATUS)SmmuBlock (&Slot->Domain);
    if (!EFI_ERROR (Status)) {
      Status = (EFI_STATUS)SmmuDetach (&Slot->Domain);
    }
    if (EFI_ERROR (Status)) {
      FirstError = Status;
      continue;
    }
    FreeDomainTables (Slot);
    ZeroMem (Slot, sizeof (*Slot));
  }
  EfiReleaseLock (&mSmmu.Lock);
  return FirstError;
}

STATIC
EFI_STATUS
SetMappingAttributeAllLocked (
  IN OUT CR_SMMU_MAPPING *Map,
  IN     UINT32           Access
  )
{
  UINT32    Previous[CR_SMMU_MAX_DOMAINS];
  UINTN     Index;
  EFI_STATUS Status;
  BOOLEAN   RollbackFailed;

  if (Map == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Previous, Map->DomainAccess, sizeof (Previous));
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (!mSmmu.Domains[Index].InUse) {
      continue;
    }
    Status = (EFI_STATUS)SmmuSetMapping (
                              &mSmmu.Domains[Index].Domain,
                              Map->IovaBase,
                              Map->PhysicalBase,
                              Map->Pages,
                              Access
                              );
    if (EFI_ERROR (Status)) {
      /* A permission update must be all-or-nothing across the SID domains. */
      RollbackFailed = FALSE;
      for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
        if (!mSmmu.Domains[Index].InUse) {
          continue;
        }
        if (EFI_ERROR ((EFI_STATUS)SmmuSetMapping (
                             &mSmmu.Domains[Index].Domain,
                             Map->IovaBase,
                             Map->PhysicalBase,
                             Map->Pages,
                             Previous[Index]))) {
          RollbackFailed = TRUE;
        }
      }
      if (RollbackFailed) {
        /* Leave no partially writable mapping if a rollback also failed. */
        for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
          if (mSmmu.Domains[Index].InUse) {
            (VOID)SmmuSetMapping (
                    &mSmmu.Domains[Index].Domain,
                    Map->IovaBase,
                    0,
                    Map->Pages,
                    0
                    );
          }
          Map->DomainAccess[Index] = 0;
        }
        Map->Access = 0;
      }
      return Status;
    }
  }
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (mSmmu.Domains[Index].InUse) {
      Map->DomainAccess[Index] = Access;
    }
  }
  Map->Access = Access;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuSetAttribute (
  IN EDKII_IOMMU_PROTOCOL *This,
  IN EFI_HANDLE             DeviceHandle,
  IN VOID                  *Mapping,
  IN UINT64                 IoMmuAccess
  )
{
  CR_SMMU_MAPPING *Map;
  UINT32           Access;
  EFI_STATUS       Status;

  (VOID)DeviceHandle;
  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.AfterExitBootServices ||
      ((IoMmuAccess & ~(EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Map = FindMapping (Mapping);
  if (Map == NULL) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_INVALID_PARAMETER;
  }
  Access = (UINT32)IoMmuAccess;
  Status = SetMappingAttributeAllLocked (Map, Access);
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuSetAttributeById (
  IN EDKII_IOMMU_PROTOCOL *This,
  IN UINT64                IommuBase,
  IN UINT32                DmaId,
  IN VOID                 *Mapping,
  IN UINT64                IoMmuAccess
  )
{
  CR_SMMU_DOMAIN  *Domain;
  CR_SMMU_MAPPING *Map;
  UINTN            DomainIndex;
  UINT32           Access;
  EFI_STATUS       Status;

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Mapping == NULL) ||
      ((IoMmuAccess & ~(EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (IommuBase != 0 && IommuBase != mSmmu.Device.Base) {
    return EFI_NOT_FOUND;
  }
  if (DmaId > 0x7FFFU) {
    return EFI_NOT_FOUND;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Domain = FindDomainBySid ((UINT16)DmaId);
  Map    = FindMapping (Mapping);
  if ((Domain == NULL) || (Map == NULL) ||
      !FindDomainIndex (Domain, &DomainIndex)) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_NOT_FOUND;
  }
  Access = (UINT32)IoMmuAccess;
  Status = (EFI_STATUS)SmmuSetMapping (
                          &Domain->Domain,
                          Map->IovaBase,
                          Map->PhysicalBase,
                          Map->Pages,
                          Access
                          );
  if (!EFI_ERROR (Status)) {
    Map->DomainAccess[DomainIndex] = Access;
    Map->Access                   = Access;
  }
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuMap (
  IN     EDKII_IOMMU_PROTOCOL  *This,
  IN     EDKII_IOMMU_OPERATION  Operation,
  IN     VOID                  *HostAddress,
  IN OUT UINTN                 *NumberOfBytes,
  OUT    EFI_PHYSICAL_ADDRESS  *DeviceAddress,
  OUT    VOID                 **Mapping
  )
{
  EFI_PHYSICAL_ADDRESS Physical;
  EFI_PHYSICAL_ADDRESS AlignedPhysical;
  EFI_PHYSICAL_ADDRESS Iova;
  UINTN                Offset;
  UINTN                Total;
  UINTN                Pages;
  UINTN                FirstPage;
  UINTN                Index;
  CR_SMMU_MAPPING     *Map;
  EFI_STATUS           Status;

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu ||
      mSmmu.AfterExitBootServices || (Operation >= EdkiiIoMmuOperationMaximum) ||
      (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (*NumberOfBytes == 0) || (DeviceAddress == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Physical        = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  Offset          = (UINTN)(Physical & (SMMU_PAGE_SIZE - 1U));
  AlignedPhysical = Physical - Offset;
  if (*NumberOfBytes > MAX_UINTN - Offset) {
    return EFI_INVALID_PARAMETER;
  }
  Total = *NumberOfBytes + Offset;
  if (Total > MAX_UINTN - (SMMU_PAGE_SIZE - 1U)) {
    return EFI_INVALID_PARAMETER;
  }
  Pages = (Total + SMMU_PAGE_SIZE - 1U) / SMMU_PAGE_SIZE;

  EfiAcquireLock (&mSmmu.Lock);
  if (!AnyDomainInUse ()) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_NOT_READY;
  }

  Map = NULL;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    if (!mSmmu.Mappings[Index].InUse) {
      Map = &mSmmu.Mappings[Index];
      break;
    }
  }
  if (Map == NULL || !FindIovaRange (Pages, &FirstPage)) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Map, sizeof (*Map));
  Map->Signature      = CR_SMMU_MAPPING_SIGNATURE;
  Map->InUse          = TRUE;
  Map->Operation      = Operation;
  Map->HostAddress    = Physical;
  Map->PhysicalBase   = AlignedPhysical;
  Map->IovaBase       = SMMU_IOVA_BASE +
                        (EFI_PHYSICAL_ADDRESS)FirstPage * SMMU_PAGE_SIZE;
  Map->NumberOfBytes  = *NumberOfBytes;
  Map->Pages          = Pages;
  Iova                = Map->IovaBase + Offset;

  Status = EFI_SUCCESS;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (!mSmmu.Domains[Index].InUse) {
      continue;
    }
    Status = (EFI_STATUS)SmmuSetMapping (
                              &mSmmu.Domains[Index].Domain,
                              Map->IovaBase,
                              Map->PhysicalBase,
                              Map->Pages,
                              0
                              );
    if (EFI_ERROR (Status)) {
      break;
    }
  }
  if (EFI_ERROR (Status)) {
    /* Revoke any domains updated before the failing one. */
    for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
      if (mSmmu.Domains[Index].InUse) {
        (VOID)SmmuSetMapping (
                &mSmmu.Domains[Index].Domain,
                Map->IovaBase,
                0,
                Map->Pages,
                0
                );
      }
    }
    ReleaseIovaRange (Map->IovaBase, Map->Pages);
    ZeroMem (Map, sizeof (*Map));
    EfiReleaseLock (&mSmmu.Lock);
    return Status;
  }

  *DeviceAddress = Iova;
  *Mapping       = Map;
  EfiReleaseLock (&mSmmu.Lock);
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuUnmap (
  IN EDKII_IOMMU_PROTOCOL *This,
  IN VOID                  *Mapping
  )
{
  CR_SMMU_MAPPING *Map;
  EFI_STATUS       Status;
  UINTN            Index;

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Map = FindMapping (Mapping);
  if (Map == NULL) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_INVALID_PARAMETER;
  }
  Status = EFI_SUCCESS;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (!mSmmu.Domains[Index].InUse) {
      continue;
    }
    Status = (EFI_STATUS)SmmuSetMapping (
                              &mSmmu.Domains[Index].Domain,
                              Map->IovaBase,
                              0,
                              Map->Pages,
                              0
                              );
    if (EFI_ERROR (Status)) {
      break;
    }
  }
  if (!EFI_ERROR (Status)) {
    ReleaseIovaRange (Map->IovaBase, Map->Pages);
    ZeroMem (Map, sizeof (*Map));
  }
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuAllocateBuffer (
  IN     EDKII_IOMMU_PROTOCOL *This,
  IN     EFI_ALLOCATE_TYPE     Type,
  IN     EFI_MEMORY_TYPE       MemoryType,
  IN     UINTN                 Pages,
  IN OUT VOID                **HostAddress,
  IN     UINT64                Attributes
  )
{
  EFI_PHYSICAL_ADDRESS Address;

  (VOID)Type;
  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Pages == 0) || (HostAddress == NULL) ||
      ((Attributes & ~EDKII_IOMMU_ATTRIBUTE_VALID_FOR_ALLOCATE_BUFFER) != 0) ||
      ((Attributes & EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE) != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if (gBS == NULL || gBS->AllocatePages == NULL) {
    return EFI_NOT_READY;
  }
  Address = (mSmmu.Device.AddressBits >= 64)
              ? MAX_UINT64
              : ((mSmmu.Device.AddressBits == 0)
                   ? 0
                   : ((1ULL << mSmmu.Device.AddressBits) - 1ULL));
  if (Address == 0 || EFI_ERROR (gBS->AllocatePages (
                                  AllocateMaxAddress,
                                  MemoryType,
                                  Pages,
                                  &Address))) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (!AddressRangeFitsDevice (Address, Pages)) {
    gBS->FreePages (Address, Pages);
    return EFI_OUT_OF_RESOURCES;
  }
  *HostAddress = (VOID *)(UINTN)Address;
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuFreeBuffer (
  IN EDKII_IOMMU_PROTOCOL *This,
  IN UINTN                 Pages,
  IN VOID                 *HostAddress
  )
{
  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Pages == 0) || (HostAddress == NULL) || (gBS == NULL) ||
      (gBS->FreePages == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  return gBS->FreePages ((EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress, Pages);
}

STATIC
VOID
EFIAPI
CrSmmuExitBootServices (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  (VOID)Context;
  mSmmu.AfterExitBootServices = TRUE;
  if ((Event != NULL) && (gBS != NULL) && (gBS->CloseEvent != NULL)) {
    gBS->CloseEvent (Event);
  }
}

STATIC
EFI_STATUS
GetTargetSmmuContext (
  OUT CrTargetSmmuContext **Target
  )
{
  CrTargetSmmuContext *Context;

  if (Target == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Context = CrTargetGetSmmuContext ();
  if ((Context == NULL) || (Context->Base == 0) || (Context->Size == 0) ||
      (Context->SegmentCount == 0) ||
      (Context->DefaultStreamCount != 0 &&
       Context->DefaultStreamIds == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Context->DefaultStreamCount > CR_SMMU_MAX_DOMAINS) {
    return EFI_OUT_OF_RESOURCES;
  }
  *Target = Context;
  return EFI_SUCCESS;
}

STATIC
VOID
InitializeSmmuMetadata (
  IN CrTargetSmmuContext *Target
  )
{
  ZeroMem (&mSmmu, sizeof (mSmmu));
  mSmmu.Target      = Target;
  mSmmu.DomainLimit = MIN ((UINTN)Target->MaximumDomains,
                           (UINTN)CR_SMMU_MAX_DOMAINS);
  if (mSmmu.DomainLimit == 0) {
    mSmmu.DomainLimit = CR_SMMU_MAX_DOMAINS;
  }
  EfiInitializeLock (&mSmmu.Lock, TPL_NOTIFY);
}

STATIC
EFI_STATUS
InitializeExternalSmmuState (
  IN EDKII_IOMMU_PROTOCOL *Existing
  )
{
  EFI_STATUS            Status;
  CrTargetSmmuContext  *Target;

  if (Existing == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Status = GetTargetSmmuContext (&Target);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  InitializeSmmuMetadata (Target);
  mSmmu.ExistingIoMmu = TRUE;

  /* A standard IOMMU producer is not proof that it configured this target's
     PCIe SIDs.  Do not probe or write the shared controller in that case: the
     existing producer's DMA mappings and this adapter's domains could not be
     coordinated.  Returning an error also keeps PciHostBridgeLib from
     enabling clocks, PHY, PERST, or bus mastering without an SMMU contract. */
  if (!Target->ExternalIoMmuOwnsPcieStreams) {
    DEBUG ((
      DEBUG_ERROR,
      "Crane SMMU: existing IOMMU owns unknown PCIe streams; "
      "refusing CR handoff\n"
      ));
    return EFI_UNSUPPORTED;
  }

  /* The external owner handles all MMIO programming in this mode. */
  mSmmu.Device.Base       = Target->Base;
  mSmmu.Device.AddressBits = 64;
  mSmmu.Initialized       = TRUE;
  DEBUG ((DEBUG_INFO, "Crane SMMU: using existing IoMmu producer %p\n", Existing));
  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
InitializeSmmuState (
  VOID
  )
{
  CR_STATUS            CrStatus;
  EFI_STATUS           Status;
  CrTargetSmmuContext *Target;

  Status = GetTargetSmmuContext (&Target);
  if (EFI_ERROR (Status)) {
    return Status;
  }
  InitializeSmmuMetadata (Target);

  CrStatus = SmmuProbe (&mSmmu.Device, Target->Base, Target->Size, &mSmmuIoOps);
  if (CR_ERROR (CrStatus)) {
    return (EFI_STATUS)CrStatus;
  }
  mSmmu.Initialized = TRUE;

  /* Stream ownership is attached by the PCIe resource callback.  The target
     list is an allow-list only; binding all SIDs here would lose the real
     segment/BDF association for multi-root platforms. */
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmmuCrDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS              Status;
  EDKII_IOMMU_PROTOCOL   *Existing;

  (VOID)SystemTable;
  mSmmuImageHandle = ImageHandle;

  /* HALIOMMUDxe may already provide a complete IOMMU implementation. */
  Existing = NULL;
  Status   = gBS->LocateProtocol (&gEdkiiIoMmuProtocolGuid, NULL,
                                  (VOID **)&Existing);
  if (!EFI_ERROR (Status) && (Existing != NULL)) {
    Status = InitializeExternalSmmuState (Existing);
  } else {
    Status = InitializeSmmuState ();
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: initialization failed: %r\n", Status));
    return Status;
  }

  if (!mSmmu.ExistingIoMmu) {
    mIoMmuProtocol.Revision         = EDKII_IOMMU_PROTOCOL_REVISION;
    mIoMmuProtocol.SetAttribute     = CrSmmuSetAttribute;
    mIoMmuProtocol.Map              = CrSmmuMap;
    mIoMmuProtocol.Unmap            = CrSmmuUnmap;
    mIoMmuProtocol.AllocateBuffer   = CrSmmuAllocateBuffer;
    mIoMmuProtocol.FreeBuffer       = CrSmmuFreeBuffer;
    mIoMmuProtocol.SetAttributeById = CrSmmuSetAttributeById;
  }

  mCrProtocol.Revision     = EFI_SMMU_CR_PROTOCOL_REVISION;
  mCrProtocol.Attach       = CrSmmuAttach;
  mCrProtocol.BlockSegment = CrSmmuBlockSegment;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  CrSmmuExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mSmmu.ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (mSmmu.ExistingIoMmu) {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &mSmmuImageHandle,
                    &gEfiSmmuCrProtocolGuid,
                    &mCrProtocol,
                    NULL
                    );
  } else {
    Status = gBS->InstallMultipleProtocolInterfaces (
                    &mSmmuImageHandle,
                    &gEdkiiIoMmuProtocolGuid,
                    &mIoMmuProtocol,
                    &gEfiSmmuCrProtocolGuid,
                    &mCrProtocol,
                    NULL
                    );
  }
  if (EFI_ERROR (Status)) {
    gBS->CloseEvent (mSmmu.ExitBootServicesEvent);
    mSmmu.ExitBootServicesEvent = NULL;
  }
  return Status;
}
