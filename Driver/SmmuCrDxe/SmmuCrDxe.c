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
#include <Library/CrDalLib.h>
#include <Library/DebugLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/IoMmu.h>
#include <Protocol/PciIo.h>

#include <Library/smmu.h>
#include <Protocol/EFISmmuCrProtocol.h>

#define CR_SMMU_MAX_DOMAINS       32U
#define CR_SMMU_MAX_MAPPINGS      128U
#define CR_SMMU_MAX_DMA_BUFFERS   128U
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
  EFI_PHYSICAL_ADDRESS      BounceAddress;
  UINTN                     NumberOfBytes;
  UINTN                     Pages;
  UINTN                     BouncePages;
  BOOLEAN                   DeviceWriteGranted;
  /* Each PCI function or explicit SID owns its permissions independently. */
  UINT32                    DomainAccess[CR_SMMU_MAX_DOMAINS];
} CR_SMMU_MAPPING;

typedef struct {
  BOOLEAN              InUse;
  BOOLEAN              Busy;
  EFI_PHYSICAL_ADDRESS Address;
  UINTN                Pages;
  UINT64               OriginalAttributes;
} CR_SMMU_DMA_BUFFER;

typedef struct {
  SmmuDevice                Device;
  CONST CrTargetSmmuContext *Target;
  CR_SMMU_DOMAIN            Domains[CR_SMMU_MAX_DOMAINS];
  CR_SMMU_MAPPING           Mappings[CR_SMMU_MAX_MAPPINGS];
  CR_SMMU_DMA_BUFFER        DmaBuffers[CR_SMMU_MAX_DMA_BUFFERS];
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
CommonBufferRangeValidLocked (
  IN EFI_PHYSICAL_ADDRESS Address,
  IN UINTN                Pages
  )
{
  EFI_PHYSICAL_ADDRESS Offset;
  UINTN                Index;
  UINTN                FirstPage;

  for (Index = 0; Index < ARRAY_SIZE (mSmmu.DmaBuffers); Index++) {
    if (!mSmmu.DmaBuffers[Index].InUse || mSmmu.DmaBuffers[Index].Busy ||
        (Address < mSmmu.DmaBuffers[Index].Address)) {
      continue;
    }
    Offset = Address - mSmmu.DmaBuffers[Index].Address;
    if ((Offset & (SMMU_PAGE_SIZE - 1U)) != 0) {
      continue;
    }
    FirstPage = (UINTN)(Offset / SMMU_PAGE_SIZE);
    if ((FirstPage <= mSmmu.DmaBuffers[Index].Pages) &&
        (Pages <= mSmmu.DmaBuffers[Index].Pages - FirstPage)) {
      return TRUE;
    }
  }

  return FALSE;
}

STATIC
VOID
PrepareMappingForDma (
  IN CR_SMMU_MAPPING *Map
  )
{
  switch (Map->Operation) {
    case EdkiiIoMmuOperationBusMasterRead:
    case EdkiiIoMmuOperationBusMasterRead64:
      WriteBackDataCacheRange (
        (VOID *)(UINTN)Map->HostAddress,
        Map->NumberOfBytes
        );
      break;

    case EdkiiIoMmuOperationBusMasterWrite:
    case EdkiiIoMmuOperationBusMasterWrite64:
      CopyMem (
        (VOID *)(UINTN)Map->BounceAddress,
        (VOID *)(UINTN)Map->HostAddress,
        Map->NumberOfBytes
        );
      WriteBackInvalidateDataCacheRange (
        (VOID *)(UINTN)Map->BounceAddress,
        Map->BouncePages * SMMU_PAGE_SIZE
        );
      break;

    default:
      break;
  }
}

STATIC
EFI_STATUS
CompleteMappingDma (
  IN OUT CR_SMMU_MAPPING *Map
  )
{
  EFI_STATUS Status;

  if ((Map->Operation != EdkiiIoMmuOperationBusMasterWrite) &&
      (Map->Operation != EdkiiIoMmuOperationBusMasterWrite64)) {
    return EFI_SUCCESS;
  }
  if ((Map->BounceAddress == 0) || (Map->BouncePages == 0) ||
      (gBS == NULL) || (gBS->FreePages == NULL)) {
    return EFI_DEVICE_ERROR;
  }

  if (Map->DeviceWriteGranted) {
    InvalidateDataCacheRange (
      (VOID *)(UINTN)Map->BounceAddress,
      Map->BouncePages * SMMU_PAGE_SIZE
      );
    CopyMem (
      (VOID *)(UINTN)Map->HostAddress,
      (VOID *)(UINTN)Map->BounceAddress,
      Map->NumberOfBytes
      );
  }

  Status = gBS->FreePages (Map->BounceAddress, Map->BouncePages);
  if (!EFI_ERROR (Status)) {
    Map->BounceAddress = 0;
    Map->BouncePages   = 0;
  }
  return Status;
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
                         EfiReservedMemoryType,
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
  Existing = FindDomainBySid (Sid);
  if (Existing != NULL) {
    /* Repeated attachment is idempotent only inside the same PCI segment. */
    if ((Existing->Segment == Segment) &&
        ((Existing->Bdf == Bdf) || (Bdf == CR_SMMU_BDF_UNKNOWN) ||
         (Existing->Bdf == CR_SMMU_BDF_UNKNOWN))) {
      if (Existing->Bdf == CR_SMMU_BDF_UNKNOWN) {
        Existing->Bdf = Bdf;
      }
      return EFI_SUCCESS;
    }
    return EFI_ACCESS_DENIED;
  }
  /* A newly attached SID would otherwise miss mappings already installed on
     the existing domains.  Cold PCIe attach is required before any DMA map. */
  if (AnyMappingInUse ()) {
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
  /* The existing standard IOMMU provider owns this mode.  Its stream
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
  BOOLEAN         Found;

  if ((This != &mCrProtocol) || !mSmmu.Initialized ||
      mSmmu.AfterExitBootServices || (mSmmu.Target == NULL) ||
      (Segment >= mSmmu.Target->SegmentCount)) {
    return EFI_INVALID_PARAMETER;
  }

  /* An existing standard IOMMU provider owns its stream/context state only
     when the target says that it owns the PCIe streams.  Otherwise the domains
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

  FirstError = EFI_SUCCESS;
  Found      = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    Slot = &mSmmu.Domains[Index];
    if (!Slot->InUse || (Slot->Segment != Segment)) {
      continue;
    }
    Found  = TRUE;
    Status = (EFI_STATUS)SmmuBlock (&Slot->Domain);
    if (!EFI_ERROR (Status)) {
      Status = (EFI_STATUS)SmmuDetach (&Slot->Domain);
    }
    if (EFI_ERROR (Status)) {
      if (!EFI_ERROR (FirstError)) {
        FirstError = Status;
      }
      continue;
    }
    FreeDomainTables (Slot);
    ZeroMem (Slot, sizeof (*Slot));
  }
  EfiReleaseLock (&mSmmu.Lock);
  return Found ? FirstError : EFI_NOT_FOUND;
}

STATIC
EFI_STATUS
SetMappingAttributeForPciDeviceLocked (
  IN OUT CR_SMMU_MAPPING *Map,
  IN     UINT16           Segment,
  IN     UINT16           Bdf,
  IN     UINT32           Access
  )
{
  UINT32    Previous[CR_SMMU_MAX_DOMAINS];
  BOOLEAN   Updated[CR_SMMU_MAX_DOMAINS];
  UINTN     Index;
  EFI_STATUS Status;
  BOOLEAN   Found;

  if (Map == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CopyMem (Previous, Map->DomainAccess, sizeof (Previous));
  ZeroMem (Updated, sizeof (Updated));
  Found = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (!mSmmu.Domains[Index].InUse ||
        (mSmmu.Domains[Index].Segment != Segment) ||
        (mSmmu.Domains[Index].Bdf != Bdf)) {
      continue;
    }
    Found  = TRUE;
    Status = (EFI_STATUS)SmmuSetMapping (
                              &mSmmu.Domains[Index].Domain,
                              Map->IovaBase,
                              Map->PhysicalBase,
                              Map->Pages,
                              Access
                              );
    if (EFI_ERROR (Status)) {
      /* Roll back only the matching domains that were changed by this call. */
      for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
        if (!Updated[Index]) {
          continue;
        }
        if (!EFI_ERROR ((EFI_STATUS)SmmuSetMapping (
                              &mSmmu.Domains[Index].Domain,
                              Map->IovaBase,
                              Map->PhysicalBase,
                              Map->Pages,
                              Previous[Index]))) {
          Map->DomainAccess[Index] = Previous[Index];
        }
      }
      for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
        if ((Map->DomainAccess[Index] & EDKII_IOMMU_ACCESS_WRITE) != 0) {
          Map->DeviceWriteGranted = TRUE;
          break;
        }
      }
      return Status;
    }
    Updated[Index]           = TRUE;
    Map->DomainAccess[Index] = Access;
  }
  if (Found && ((Access & EDKII_IOMMU_ACCESS_WRITE) != 0)) {
    Map->DeviceWriteGranted = TRUE;
  }
  return Found ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

STATIC
BOOLEAN
MappingAccessSupported (
  IN CONST CR_SMMU_MAPPING *Map,
  IN UINT32                 Access
  )
{
  if (Map == NULL) {
    return FALSE;
  }

  switch (Map->Operation) {
    case EdkiiIoMmuOperationBusMasterRead:
    case EdkiiIoMmuOperationBusMasterRead64:
      return (Access & ~EDKII_IOMMU_ACCESS_READ) == 0;

    case EdkiiIoMmuOperationBusMasterWrite:
    case EdkiiIoMmuOperationBusMasterWrite64:
      return (Access & ~EDKII_IOMMU_ACCESS_WRITE) == 0;

    case EdkiiIoMmuOperationBusMasterCommonBuffer:
    case EdkiiIoMmuOperationBusMasterCommonBuffer64:
      return TRUE;

    default:
      return FALSE;
  }
}

STATIC
EFI_STATUS
GetPciLocation (
  IN  EFI_HANDLE DeviceHandle,
  OUT UINT16    *Segment,
  OUT UINT16    *Bdf
  )
{
  EFI_PCI_IO_PROTOCOL *PciIo;
  EFI_STATUS           Status;
  UINTN                SegmentNumber;
  UINTN                BusNumber;
  UINTN                DeviceNumber;
  UINTN                FunctionNumber;

  if ((DeviceHandle == NULL) || (Segment == NULL) || (Bdf == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((gBS == NULL) || (gBS->HandleProtocol == NULL)) {
    return EFI_NOT_READY;
  }

  PciIo  = NULL;
  Status = gBS->HandleProtocol (
                  DeviceHandle,
                  &gEfiPciIoProtocolGuid,
                  (VOID **)&PciIo
                  );
  if (EFI_ERROR (Status) || (PciIo == NULL) || (PciIo->GetLocation == NULL)) {
    return (Status == EFI_INVALID_PARAMETER) ? Status : EFI_UNSUPPORTED;
  }
  Status = PciIo->GetLocation (
                    PciIo,
                    &SegmentNumber,
                    &BusNumber,
                    &DeviceNumber,
                    &FunctionNumber
                    );
  if (EFI_ERROR (Status)) {
    return EFI_UNSUPPORTED;
  }
  if ((SegmentNumber > MAX_UINT16) || (BusNumber > MAX_UINT8) ||
      (DeviceNumber > 31U) || (FunctionNumber > 7U)) {
    return EFI_UNSUPPORTED;
  }

  *Segment = (UINT16)SegmentNumber;
  *Bdf     = (UINT16)((BusNumber << 8) | (DeviceNumber << 3) |
                     FunctionNumber);
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
  UINT16           Segment;
  UINT16           Bdf;

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.AfterExitBootServices ||
      ((IoMmuAccess & ~(EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  Status = GetPciLocation (DeviceHandle, &Segment, &Bdf);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Map = FindMapping (Mapping);
  if (Map == NULL) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_INVALID_PARAMETER;
  }
  Access = (UINT32)IoMmuAccess;
  if (!MappingAccessSupported (Map, Access)) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_UNSUPPORTED;
  }
  Status = SetMappingAttributeForPciDeviceLocked (Map, Segment, Bdf, Access);
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
  if (IommuBase != mSmmu.Device.Base) {
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
  if (!MappingAccessSupported (Map, Access)) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_UNSUPPORTED;
  }
  Status = (EFI_STATUS)SmmuSetMapping (
                          &Domain->Domain,
                          Map->IovaBase,
                          Map->PhysicalBase,
                          Map->Pages,
                          Access
                          );
  if (!EFI_ERROR (Status)) {
    Map->DomainAccess[DomainIndex] = Access;
    if ((Access & EDKII_IOMMU_ACCESS_WRITE) != 0) {
      Map->DeviceWriteGranted = TRUE;
    }
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
  EFI_PHYSICAL_ADDRESS Maximum;
  UINTN                Offset;
  UINTN                Total;
  UINTN                Pages;
  UINTN                FirstPage;
  UINTN                Index;
  CR_SMMU_MAPPING     *Map;
  EFI_STATUS           Status;
  BOOLEAN              CleanupFailed;
  BOOLEAN              UseBounce;

  if (DeviceAddress != NULL) {
    *DeviceAddress = 0;
  }
  if (Mapping != NULL) {
    *Mapping = NULL;
  }
  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu ||
      mSmmu.AfterExitBootServices ||
      ((UINT32)Operation >= EdkiiIoMmuOperationMaximum) ||
      (HostAddress == NULL) || (NumberOfBytes == NULL) ||
      (*NumberOfBytes == 0) || (DeviceAddress == NULL) || (Mapping == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Physical        = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  if ((UINTN)HostAddress > MAX_UINTN - (*NumberOfBytes - 1U)) {
    return EFI_INVALID_PARAMETER;
  }
  UseBounce = (Operation == EdkiiIoMmuOperationBusMasterWrite) ||
              (Operation == EdkiiIoMmuOperationBusMasterWrite64);
  if (UseBounce) {
    Offset = 0;
    if (*NumberOfBytes > MAX_UINTN - (SMMU_PAGE_SIZE - 1U)) {
      return EFI_INVALID_PARAMETER;
    }
    Pages = (*NumberOfBytes + SMMU_PAGE_SIZE - 1U) / SMMU_PAGE_SIZE;
    if ((gBS == NULL) || (gBS->AllocatePages == NULL) ||
        (gBS->FreePages == NULL)) {
      return EFI_NOT_READY;
    }
    if (mSmmu.Device.AddressBits >= 64) {
      Maximum = MAX_UINT64;
    } else if (mSmmu.Device.AddressBits == 0) {
      return EFI_UNSUPPORTED;
    } else {
      Maximum = (1ULL << mSmmu.Device.AddressBits) - 1ULL;
    }
    AlignedPhysical = Maximum;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    Pages,
                    &AlignedPhysical
                    );
    if (EFI_ERROR (Status)) {
      return Status;
    }
    if ((AlignedPhysical == 0) ||
        !AddressRangeFitsDevice (AlignedPhysical, Pages)) {
      (VOID)gBS->FreePages (AlignedPhysical, Pages);
      return EFI_OUT_OF_RESOURCES;
    }
    Physical = AlignedPhysical;
  } else {
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
    if (!AddressRangeFitsDevice (AlignedPhysical, Pages)) {
      return EFI_UNSUPPORTED;
    }
  }

  EfiAcquireLock (&mSmmu.Lock);
  if (!AnyDomainInUse ()) {
    EfiReleaseLock (&mSmmu.Lock);
    if (UseBounce) {
      (VOID)gBS->FreePages (AlignedPhysical, Pages);
    }
    return EFI_NOT_READY;
  }
  if (((Operation == EdkiiIoMmuOperationBusMasterCommonBuffer) ||
       (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer64)) &&
      !CommonBufferRangeValidLocked (AlignedPhysical, Pages)) {
    EfiReleaseLock (&mSmmu.Lock);
    return EFI_UNSUPPORTED;
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
    if (UseBounce) {
      (VOID)gBS->FreePages (AlignedPhysical, Pages);
    }
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
  if (UseBounce) {
    Map->HostAddress   = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
    Map->BounceAddress = AlignedPhysical;
    Map->BouncePages   = Pages;
  }
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
    CleanupFailed = FALSE;
    for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
      if (mSmmu.Domains[Index].InUse) {
        if (CR_ERROR (SmmuSetMapping (
                        &mSmmu.Domains[Index].Domain,
                        Map->IovaBase,
                        0,
                        Map->Pages,
                        0
                        ))) {
          CleanupFailed = TRUE;
        }
      }
    }
    if (!CleanupFailed) {
      ReleaseIovaRange (Map->IovaBase, Map->Pages);
      ZeroMem (Map, sizeof (*Map));
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "Crane SMMU: retaining failed map at IOVA 0x%Lx\n",
        Map->IovaBase
        ));
    }
    EfiReleaseLock (&mSmmu.Lock);
    if (UseBounce && !CleanupFailed) {
      (VOID)gBS->FreePages (AlignedPhysical, Pages);
    }
    return Status;
  }

  PrepareMappingForDma (Map);
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
  EFI_STATUS       FirstError;
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
  FirstError = EFI_SUCCESS;
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
      if (!EFI_ERROR (FirstError)) {
        FirstError = Status;
      }
      continue;
    }
    Map->DomainAccess[Index] = 0;
  }
  if (!EFI_ERROR (FirstError)) {
    FirstError = CompleteMappingDma (Map);
    if (!EFI_ERROR (FirstError)) {
      ReleaseIovaRange (Map->IovaBase, Map->Pages);
      ZeroMem (Map, sizeof (*Map));
    }
  }
  EfiReleaseLock (&mSmmu.Lock);
  return FirstError;
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
  EFI_PHYSICAL_ADDRESS            Address;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS                      Status;
  CR_SMMU_DMA_BUFFER             *Slot;
  UINT64                          UncachedAttributes;
  UINTN                           Index;
  UINTN                           Size;

  (VOID)Type;
  if (HostAddress != NULL) {
    *HostAddress = NULL;
  }
  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Pages == 0) || (Pages > MAX_UINTN / SMMU_PAGE_SIZE) ||
      (HostAddress == NULL) ||
      ((MemoryType != EfiBootServicesData) &&
       (MemoryType != EfiRuntimeServicesData))) {
    return EFI_INVALID_PARAMETER;
  }
  if (((Attributes & ~EDKII_IOMMU_ATTRIBUTE_VALID_FOR_ALLOCATE_BUFFER) != 0) ||
      ((Attributes & (EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE |
                      EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED)) != 0)) {
    return EFI_UNSUPPORTED;
  }
  if ((gBS == NULL) || (gBS->AllocatePages == NULL) ||
      (gBS->FreePages == NULL) || (gDS == NULL) ||
      (gDS->GetMemorySpaceDescriptor == NULL) ||
      (gDS->SetMemorySpaceAttributes == NULL)) {
    return EFI_NOT_READY;
  }

  Slot = NULL;
  EfiAcquireLock (&mSmmu.Lock);
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.DmaBuffers); Index++) {
    if (!mSmmu.DmaBuffers[Index].InUse && !mSmmu.DmaBuffers[Index].Busy) {
      Slot       = &mSmmu.DmaBuffers[Index];
      Slot->Busy = TRUE;
      break;
    }
  }
  EfiReleaseLock (&mSmmu.Lock);
  if (Slot == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Address = (mSmmu.Device.AddressBits >= 64)
              ? MAX_UINT64
              : ((mSmmu.Device.AddressBits == 0)
                   ? 0
                   : ((1ULL << mSmmu.Device.AddressBits) - 1ULL));
  if (Address == 0) {
    Status = EFI_UNSUPPORTED;
    goto ReleaseSlot;
  }
  Status = gBS->AllocatePages (
                  AllocateMaxAddress,
                  MemoryType,
                  Pages,
                  &Address
                  );
  if (EFI_ERROR (Status)) {
    goto ReleaseSlot;
  }
  if (!AddressRangeFitsDevice (Address, Pages)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreePages;
  }

  Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
  if (EFI_ERROR (Status)) {
    goto FreePages;
  }
  if ((Descriptor.Capabilities & EFI_MEMORY_UC) == 0) {
    Status = EFI_UNSUPPORTED;
    goto FreePages;
  }

  Size               = Pages * SMMU_PAGE_SIZE;
  UncachedAttributes =
    (Descriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC;
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Address, Size);
  Status = gDS->SetMemorySpaceAttributes (
                  Address,
                  Size,
                  UncachedAttributes
                  );
  if (EFI_ERROR (Status)) {
    goto FreePages;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Slot->Address            = Address;
  Slot->Pages              = Pages;
  Slot->OriginalAttributes = Descriptor.Attributes;
  Slot->InUse              = TRUE;
  Slot->Busy               = FALSE;
  EfiReleaseLock (&mSmmu.Lock);
  *HostAddress = (VOID *)(UINTN)Address;
  return EFI_SUCCESS;

FreePages:
  (VOID)gBS->FreePages (Address, Pages);
ReleaseSlot:
  EfiAcquireLock (&mSmmu.Lock);
  ZeroMem (Slot, sizeof (*Slot));
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
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
  CR_SMMU_DMA_BUFFER *Slot;
  EFI_PHYSICAL_ADDRESS Address;
  EFI_STATUS           Status;
  EFI_STATUS           RestoreStatus;
  UINT64               UncachedAttributes;
  BOOLEAN              KeepBusy;
  UINTN                Index;
  UINTN                Size;

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Pages == 0) || (Pages > MAX_UINTN / SMMU_PAGE_SIZE) ||
      (HostAddress == NULL) ||
      (((UINTN)HostAddress & (SMMU_PAGE_SIZE - 1U)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }
  if ((gBS == NULL) || (gBS->FreePages == NULL) || (gDS == NULL) ||
      (gDS->SetMemorySpaceAttributes == NULL)) {
    return EFI_NOT_READY;
  }

  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  Slot    = NULL;
  EfiAcquireLock (&mSmmu.Lock);
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.DmaBuffers); Index++) {
    if (mSmmu.DmaBuffers[Index].InUse &&
        !mSmmu.DmaBuffers[Index].Busy &&
        (mSmmu.DmaBuffers[Index].Address == Address) &&
        (mSmmu.DmaBuffers[Index].Pages == Pages)) {
      Slot       = &mSmmu.DmaBuffers[Index];
      Slot->Busy = TRUE;
      break;
    }
  }
  EfiReleaseLock (&mSmmu.Lock);
  if (Slot == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  Size = Pages * SMMU_PAGE_SIZE;
  KeepBusy = FALSE;
  WriteBackInvalidateDataCacheRange (HostAddress, Size);
  Status = gDS->SetMemorySpaceAttributes (
                  Address,
                  Size,
                  Slot->OriginalAttributes
                  );
  if (!EFI_ERROR (Status)) {
    Status = gBS->FreePages (Address, Pages);
    if (EFI_ERROR (Status)) {
      UncachedAttributes =
        (Slot->OriginalAttributes & ~EFI_CACHE_ATTRIBUTE_MASK) | EFI_MEMORY_UC;
      RestoreStatus = gDS->SetMemorySpaceAttributes (
                            Address,
                            Size,
                            UncachedAttributes
                            );
      if (EFI_ERROR (RestoreStatus)) {
        Status   = RestoreStatus;
        KeepBusy = TRUE;
      }
    }
  }

  EfiAcquireLock (&mSmmu.Lock);
  if (EFI_ERROR (Status)) {
    Slot->Busy = KeepBusy;
  } else {
    ZeroMem (Slot, sizeof (*Slot));
  }
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC
VOID
EFIAPI
CrSmmuExitBootServices (
  IN EFI_EVENT Event,
  IN VOID      *Context
  )
{
  EFI_STATUS Status;
  UINTN      Index;

  (VOID)Context;
  mSmmu.AfterExitBootServices = TRUE;
  if (mSmmu.Initialized && !mSmmu.ExistingIoMmu) {
    EfiAcquireLock (&mSmmu.Lock);
    for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
      if (!mSmmu.Domains[Index].InUse) {
        continue;
      }
      Status = (EFI_STATUS)SmmuBlock (&mSmmu.Domains[Index].Domain);
      if (EFI_ERROR (Status)) {
        DEBUG ((
          DEBUG_ERROR,
          "Crane SMMU: failed to block SID 0x%x at ExitBootServices: %r\n",
          mSmmu.Domains[Index].Sid,
          Status
          ));
      }
    }
    EfiReleaseLock (&mSmmu.Lock);
  }
  if ((Event != NULL) && (gBS != NULL) && (gBS->CloseEvent != NULL)) {
    gBS->CloseEvent (Event);
  }
  mSmmu.ExitBootServicesEvent = NULL;
}

STATIC
EFI_STATUS
GetTargetSmmuContext (
  OUT CONST CrTargetSmmuContext **Target
  )
{
  CONST CrTargetSmmuContext *Context;

  if (Target == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Context = CrDalGetSmmuContext ();
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
  IN CONST CrTargetSmmuContext *Target
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
  CONST CrTargetSmmuContext *Target;

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
  CONST CrTargetSmmuContext *Target;

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

  /* A standard IOMMU protocol producer may already own the controller. */
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
