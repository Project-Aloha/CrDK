/** @file
  Crane SMMUv2/MMU-500 adapter for the EDKII IOMMU protocol.

  An existing EDKII IOMMU provider may own firmware clients before this
  driver is dispatched. Without an explicit target contract proving that the
  provider owns the PCIe streams, coexistence is rejected and PCIe cold
  initialization remains unavailable. When no external producer is
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
#include <Library/PcdLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiLib.h>
#include <Protocol/IoMmu.h>
#include <Protocol/PciIo.h>
#include <Protocol/EFIDmaIdentityCrProtocol.h>

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
  BOOLEAN             PreservedAtExit;
  UINT16              Segment;
  UINT16              Bdf;
  UINT16              Sid;
  VOID               *Tables;
  EFI_PHYSICAL_ADDRESS TableAddress;
  SmmuDomain          Domain;
  VOID               *UsbOwner;
  EFI_SMMU_CR_USB_VERIFY_STOPPED UsbStopped;
} CR_SMMU_DOMAIN;

typedef struct _CR_SMMU_DISPLAY_BUFFER CR_SMMU_DISPLAY_BUFFER;

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
  BOOLEAN                   PreserveAtExit;
  BOOLEAN                   CachedCommonBuffer;
  CR_SMMU_DISPLAY_BUFFER    *DisplayOwner;
  /* Each PCI function or explicit SID owns its permissions independently. */
  UINT32                    DomainAccess[CR_SMMU_MAX_DOMAINS];
  UINT32                    ExitDomainAccess[CR_SMMU_MAX_DOMAINS];
} CR_SMMU_MAPPING;

typedef struct {
  BOOLEAN              InUse;
  BOOLEAN              Busy;
  EFI_PHYSICAL_ADDRESS Address;
  UINTN                Pages;
  UINT64               OriginalAttributes;
  UINT64               AllocationAttributes;
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
  BOOLEAN                   ExitPreparing;
  BOOLEAN                   ExitComplete;
  BOOLEAN                   ExitObserversActive;
  EFI_STATUS                ExitStatus;
  struct {
    VOID *Owner;
    EFI_SMMU_CR_EXIT_OBSERVER Callback;
    VOID *Context;
  } ExitObservers[8];
  struct {
    VOID *Owner;
    EFI_SMMU_CR_EXIT_PREPARE Callback;
    VOID *Context;
  } ExitPrepareObservers[8];
  BOOLEAN                   ExistingIoMmu;
  EFI_EVENT                 ExitBootServicesEvent;
} CR_SMMU_STATE;

STATIC CR_SMMU_STATE          mSmmu;
STATIC EFI_HANDLE             mSmmuImageHandle;
STATIC EDKII_IOMMU_PROTOCOL   mIoMmuProtocol;
STATIC EFI_SMMU_CR_PROTOCOL   mCrProtocol;

#include "SmmuDisplayPrivate.h"

STATIC
BOOLEAN
AddressRangeFitsBits (
  IN UINT8                Bits,
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
  if (Bits >= 64) {
    Limit = MAX_UINT64;
  } else if (Bits == 0) {
    return FALSE;
  } else {
    Limit = (1ULL << Bits) - 1ULL;
  }
  return (Address <= Limit) && ((Bytes - 1ULL) <= (Limit - Address));
}

STATIC
BOOLEAN
AddressRangeFitsDevice (
  IN EFI_PHYSICAL_ADDRESS Address,
  IN UINTN                Pages
  )
{
  /* Stage-1 translations produce an IPA. ID2.IAS bounds that address. */
  return AddressRangeFitsBits (mSmmu.Device.Stage1OutputAddressBits,
                               Address, Pages);
}

STATIC
BOOLEAN
AddressRangeFitsTable (
  IN EFI_PHYSICAL_ADDRESS Address,
  IN UINTN                Pages
  )
{
  /* The page-table walker emits physical addresses and is bounded by OAS. */
  return AddressRangeFitsBits (mSmmu.Device.AddressBits, Address, Pages);
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
BOOLEAN
SidCoherent (
  IN UINT16 Sid
  )
{
  UINT16 Index;

  if (mSmmu.Target != NULL) {
    for (Index = 0; Index < mSmmu.Target->CoherentStreamCount; Index++) {
      if (mSmmu.Target->CoherentStreamIds[Index] == Sid) {
        return TRUE;
      }
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

/* Caller holds Lock. Only permissions in the affected domain prevent detach. */
STATIC BOOLEAN
DomainMappingInUse (UINTN DomainIndex)
{
  UINTN Index;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    if (mSmmu.Mappings[Index].InUse &&
        mSmmu.Mappings[Index].DomainAccess[DomainIndex] != 0) return TRUE;
  }
  return FALSE;
}

STATIC BOOLEAN
RangesOverlap (UINT64 First, UINT64 FirstSize, UINT64 Second, UINT64 SecondSize)
{
  if (FirstSize == 0 || SecondSize == 0) return FALSE;
  return First <= Second ? Second - First < FirstSize : First - Second < SecondSize;
}

/* Include permissionless and failed-unmap records: their storage remains owned.
   Device-write mappings may retain both caller storage and a private bounce. */
STATIC BOOLEAN
BufferMappingInUse (UINT64 Address, UINT64 Size)
{
  UINTN Index;
  CR_SMMU_MAPPING *Map;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    Map = &mSmmu.Mappings[Index];
    if (!Map->InUse) continue;
    if (RangesOverlap (Address, Size, Map->PhysicalBase,
                       (UINT64)Map->Pages * SMMU_PAGE_SIZE) ||
        RangesOverlap (Address, Size, Map->HostAddress, Map->NumberOfBytes) ||
        RangesOverlap (Address, Size, Map->BounceAddress,
                       (UINT64)Map->BouncePages * SMMU_PAGE_SIZE)) return TRUE;
  }
  return FALSE;
}

STATIC
CR_SMMU_DMA_BUFFER *
FindCommonBufferLocked (
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
      return &mSmmu.DmaBuffers[Index];
    }
  }

  return NULL;
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AllocateDomainTables stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }

  if (mSmmu.Device.AddressBits >= 64) {
    Maximum = MAX_UINT64;
  } else if (mSmmu.Device.AddressBits == 0) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AllocateDomainTables stage=address-width status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
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
  if (Status != EFI_SUCCESS) {
    /* A warning or a changed output on failure cannot prove whether pages
       were allocated. Retain that slot without touching or freeing backing. */
    if (!EFI_ERROR(Status) || Address != Maximum) {
      Slot->TableAddress = Address;
      Slot->Tables = (VOID *)(UINTN)Address;
      Slot->Poisoned = TRUE;
    }
    DEBUG ((DEBUG_ERROR,
      "Crane SMMU: AllocatePages %u table pages below 0x%lx failed: %r\n",
      SMMU_TABLE_PAGES, Maximum, Status));
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AllocateDomainTables stage=allocate-pages status=%r\n", (EFI_STATUS)(Status)));
    return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
  }
  if (DisplayAllocationOverlap(Address, (UINT64)SMMU_TABLE_PAGES * SMMU_PAGE_SIZE)) {
    Slot->TableAddress = Address;
    Slot->Tables = (VOID *)(UINTN)Address;
    Slot->Poisoned = TRUE;
    return EFI_ACCESS_DENIED;
  }
  if (((Address & (SMMU_PAGE_SIZE - 1U)) != 0) ||
      !AddressRangeFitsTable (Address, SMMU_TABLE_PAGES)) {
    DEBUG ((DEBUG_ERROR,
      "Crane SMMU: table address 0x%lx outside %u-bit aperture\n",
      Address, mSmmu.Device.AddressBits));
    gBS->FreePages (Address, SMMU_TABLE_PAGES);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AllocateDomainTables stage=table-aperture status=%r\n", (EFI_STATUS)(EFI_OUT_OF_RESOURCES)));
    return EFI_OUT_OF_RESOURCES;
  }

  DEBUG ((DEBUG_INIT, "Crane SMMU: table allocation PA=0x%Lx pages=%u walker-aperture=%u\n", Address, SMMU_TABLE_PAGES, mSmmu.Device.AddressBits));
  Slot->TableAddress = Address;
  Slot->Tables       = (VOID *)(UINTN)Address;
  ZeroMem (Slot->Tables, SMMU_TABLE_PAGES * SMMU_PAGE_SIZE);
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: AllocateDomainTables stage=complete status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
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

/* Read-only register qualification permits retained DMA grants during HCI
   retries. It never requires empty tables or revokes a descriptor pool. */
STATIC BOOLEAN OwnedStoppedDomainValid (CR_SMMU_DOMAIN *Slot)
{
  SmmuDomain *Domain = &Slot->Domain;
  SmmuDevice *Device = &mSmmu.Device;
  UINT64 Cb = Domain->BankBase;
  UINT32 ExpectedActlr = 0;
  BOOLEAN HasActlr = FALSE;
  if (!Domain->Active || Domain->Failed || Device->SyncFailed ||
      Domain->Device != Device || Device->Io.Read32 == NULL ||
      Domain->Bank >= Device->BankCount || Domain->Bank >= 256 ||
      Domain->Stream >= Device->StreamCount || Domain->Stream >= 128 ||
      Domain->Asid >= 256 || !Device->ReservedBanks[Domain->Bank] ||
      !Device->ReservedStreams[Domain->Stream] || !Device->ReservedAsids[Domain->Asid] ||
      Cb != Device->ContextBase + (UINT64)Domain->Bank * Device->PageSize) return FALSE;
  if (Device->ActlrSettingCount != 0 && Device->ActlrSettings == NULL) return FALSE;
  for (UINTN Index = 0; Index < Device->ActlrSettingCount; Index++) {
    CONST SmmuActlrSetting *Entry = &Device->ActlrSettings[Index];
    if (((Domain->Sid ^ Entry->Sid) & ~(UINT32)Entry->Mask) != 0) continue;
    if (HasActlr && ExpectedActlr != Entry->Value) return FALSE;
    ExpectedActlr = Entry->Value; HasActlr = TRUE;
  }
  if (HasActlr != Domain->OwnsActlr) return FALSE;
#define STOP_READ(Address) Device->Io.Read32 (Device->Io.Cookie, (Address))
  BOOLEAN Valid = ((STOP_READ(Device->Base) & (BIT0 | BIT3)) == 0) &&
      STOP_READ(Device->Base + 0x800 + 4U * Domain->Stream) == (BIT31 | Domain->Sid) &&
      STOP_READ(Device->Base + 0xC00 + 4U * Domain->Stream) == Domain->Bank &&
      (STOP_READ(Cb) & ~Device->SctlrReadbackIgnoreMask) == (0x1027U & ~Device->SctlrReadbackIgnoreMask) &&
      STOP_READ(Device->Base + Device->PageSize + 4U * Domain->Bank) == 0x1F300U &&
      STOP_READ(Device->Base + Device->PageSize + 0x800 + 4U * Domain->Bank) == 1 &&
      STOP_READ(Cb + 0x30) == ((1U << 23) | 0x3500U | ((64U - Device->TranslationAddressBits) & 0x3FU)) &&
      (STOP_READ(Cb + 0x10) & 0x3801FU) == (0x38010U | Device->Stage1OutputAddressEncoding) &&
      (STOP_READ(Cb + 0x20) | ((UINT64)STOP_READ(Cb + 0x24) << 32)) ==
          (Domain->TableAddress | ((UINT64)Domain->Asid << 48)) &&
      (STOP_READ(Cb + 0x28) | ((UINT64)STOP_READ(Cb + 0x2C) << 32)) == ((UINT64)Domain->Asid << 48) &&
      STOP_READ(Cb + 0x38) == (Domain->CacheCoherent ? 0xFFU : 0x44U) && STOP_READ(Cb + 0x3C) == 0 &&
      (!HasActlr || STOP_READ(Cb + 4) == ExpectedActlr);
  /* Every other matching route or native-bank reference is an ownership
     conflict; even a disabled bank cannot be silently shared on retry. */
  for (UINT16 Index = 0; Valid && Index < Device->StreamCount; Index++) {
    if (Index == Domain->Stream) continue;
    UINT32 Smr = STOP_READ(Device->Base + 0x800 + 4U * Index);
    if ((Smr & BIT31) == 0) continue;
    UINT32 S2cr = STOP_READ(Device->Base + 0xC00 + 4U * Index);
    UINT32 Mask = (Smr >> 16) & 0x7FFFU;
    if (((Domain->Sid ^ Smr) & ~Mask & 0xFFFFU) == 0 ||
        ((S2cr & 0x30000U) == 0 && (S2cr & 0xFFU) == Domain->Bank)) Valid = FALSE;
  }
#undef STOP_READ
  return Valid;
}

STATIC EFI_STATUS AttachStreamWithStopLocked (
  IN UINT16 Segment,
  IN UINT16 Bdf,
  IN UINT16 Sid,
  IN SmmuVerifyStopped Stopped,
  IN VOID *StoppedContext,
  IN BOOLEAN DisplayAuthorized
  )
{
  CR_SMMU_DOMAIN *Existing;
  CR_SMMU_DOMAIN *Slot;
  EFI_STATUS      Status;
  UINTN           Index;

  if (Stopped != NULL) {
    Status = (EFI_STATUS)Stopped (StoppedContext);
    if (Status != EFI_SUCCESS) return EFI_ERROR(Status) ? Status : EFI_NOT_READY;
  }

  DEBUG ((DEBUG_INIT, "Crane SMMU: attach identity segment=%u BDF=0x%x SID=0x%x external=%u\n", Segment, Bdf, Sid, mSmmu.ExistingIoMmu));
  if (!(DisplayAuthorized ? DisplaySid(Sid) : SidAllowed(Sid) && !DisplaySid(Sid))) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AttachStreamLocked stage=allow-list status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
    return EFI_UNSUPPORTED;
  }
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (mSmmu.Domains[Index].Poisoned &&
        (mSmmu.Domains[Index].Sid == Sid)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: AttachStreamLocked stage=poisoned-SID status=%r\n", (EFI_STATUS)(EFI_DEVICE_ERROR)));
      return EFI_DEVICE_ERROR;
    }
  }
  Existing = FindDomainBySid (Sid);
  if (Existing != NULL) {
    /* Repeated attachment is idempotent only inside the same PCI segment. */
    /* A SID cannot silently change PCI function or cross the on-chip
       (unknown-BDF) namespace. Shared-SID ownership needs an explicit API. */
    if ((Existing->Segment == Segment) && (Existing->Bdf == Bdf)) {
      if (Stopped != NULL && !OwnedStoppedDomainValid (Existing)) {
        Existing->Domain.Failed = TRUE;
        Existing->Poisoned = TRUE;
        return EFI_DEVICE_ERROR;
      }
      DEBUG ((DEBUG_VERBOSE, "Crane SMMU: AttachStreamLocked stage=already-attached status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
      return EFI_SUCCESS;
    }
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AttachStreamLocked stage=identity-conflict status=%r\n", (EFI_STATUS)(EFI_ACCESS_DENIED)));
    return EFI_ACCESS_DENIED;
  }
  /* Each new domain starts with a zeroed page table. Existing IOVAs remain
     inaccessible to it until SetAttribute grants access for this SID. This
     permits long-lived display/UFS mappings while other devices attach. */

  Slot = NULL;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    if (!mSmmu.Domains[Index].InUse && !mSmmu.Domains[Index].Poisoned) {
      Slot = &mSmmu.Domains[Index];
      break;
    }
  }
  if ((Slot == NULL) || (Index >= mSmmu.DomainLimit)) {
    DEBUG ((DEBUG_ERROR,
      "Crane SMMU: software domain limit %u exhausted for SID=0x%x\n",
      (UINT32)mSmmu.DomainLimit, Sid));
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AttachStreamLocked stage=domain-slots status=%r\n", (EFI_STATUS)(EFI_OUT_OF_RESOURCES)));
    return EFI_OUT_OF_RESOURCES;
  }

  ZeroMem (Slot, sizeof (*Slot));
  Slot->Segment = Segment;
  Slot->Bdf     = Bdf;
  Slot->Sid     = Sid;
  Status = AllocateDomainTables (Slot);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AttachStreamLocked stage=table-allocation status=%r\n", (EFI_STATUS)(Status)));
    return Status;
  }

  if (Stopped != NULL) {
    Status = (EFI_STATUS)SmmuAcquireStoppedStream (
        &mSmmu.Device, &Slot->Domain, Sid, Slot->Tables, Slot->TableAddress,
        SidCoherent (Sid), Stopped, StoppedContext);
  } else {
    Status = (EFI_STATUS)(SidCoherent (Sid) ? SmmuAttachCoherent : SmmuAttach) (
                          &mSmmu.Device,
                          &Slot->Domain,
                          Sid,
                          Slot->Tables,
                          Slot->TableAddress
                          );
  }
  if (EFI_ERROR (Status)) {
    if (Status == EFI_OUT_OF_RESOURCES) {
      CONST CHAR8 *Reason;
      switch (mSmmu.Device.LastAttachResource) {
        case SmmuAttachResourceStream: Reason = "no free SMR"; break;
        case SmmuAttachResourceBank:   Reason = "no free context bank"; break;
        case SmmuAttachResourceAsid:   Reason = "no free ASID"; break;
        default:                       Reason = "unknown"; break;
      }
      DEBUG ((DEBUG_ERROR,
        "Crane SMMU: SID=0x%x %a (SMR=%u CB=%u stage2=%u ID0=0x%08x ID1=0x%08x ID2=0x%08x)\n",
        Sid, Reason, mSmmu.Device.StreamCount, mSmmu.Device.BankCount,
        mSmmu.Device.Stage2BankCount,
        mSmmu.Device.Io.Read32 (mSmmu.Device.Io.Cookie, mSmmu.Device.Base + 0x20),
        mSmmu.Device.Io.Read32 (mSmmu.Device.Io.Cookie, mSmmu.Device.Base + 0x24),
        mSmmu.Device.Io.Read32 (mSmmu.Device.Io.Cookie, mSmmu.Device.Base + 0x28)));
      DEBUG_CODE_BEGIN ();
      if (mSmmu.Device.Io.Read32 != NULL) {
        SmmuDevice *Device = &mSmmu.Device;
        BOOLEAN Referenced[256] = { FALSE };
        UINT16 Bank;
        UINT32 Smr;
        UINT32 S2cr;
        UINT32 Enabled = 0;
        UINT32 BoundDisabled = 0;
        UINT32 Available = 0;
        UINT32 SoftwareReserved = 0;

        for (Bank = 0; Bank < Device->StreamCount; Bank++) {
          Smr = Device->Io.Read32 (Device->Io.Cookie,
                                  Device->Base + 0x800U + 4U * Bank);
          S2cr = Device->Io.Read32 (Device->Io.Cookie,
                                   Device->Base + 0xC00U + 4U * Bank);
          if (((Smr & BIT31) != 0) || Device->ReservedStreams[Bank]) {
            DEBUG ((DEBUG_ERROR, "Crane SMMU: resource SMR[%u]=0x%08x S2CR=0x%08x software-reserved=%u\n", Bank, Smr, S2cr, Device->ReservedStreams[Bank]));
          }
          if (((Smr & BIT31) != 0) && ((S2cr & 0x30000U) == 0)) {
            Referenced[S2cr & 0xFFU] = TRUE;
          }
        }
        DEBUG ((DEBUG_ERROR, "Crane SMMU: reserved stage2 CBs=[0,%u) final-CB=%u\n", Device->Stage2BankCount, Device->BankCount - 1U));
        for (Bank = Device->Stage2BankCount; Bank + 1U < Device->BankCount; Bank++) {
          UINT64 Cb = Device->ContextBase + (UINT64)Bank * Device->PageSize;
          UINT32 Sctlr = Device->Io.Read32 (Device->Io.Cookie, Cb);
          DEBUG ((DEBUG_ERROR, "Crane SMMU: resource CB[%u] SCTLR=0x%08x CBAR=0x%08x CBA2R=0x%08x referenced=%u software-reserved=%u\n", Bank, Sctlr, Device->Io.Read32 (Device->Io.Cookie, Device->Base + Device->PageSize + 4U * Bank), Device->Io.Read32 (Device->Io.Cookie, Device->Base + Device->PageSize + 0x800U + 4U * Bank), Referenced[Bank], Device->ReservedBanks[Bank]));
          if (Device->ReservedBanks[Bank]) {
            SoftwareReserved++;
          }
          if ((Sctlr & BIT0) != 0) {
            Enabled++;
          } else if (Referenced[Bank]) {
            BoundDisabled++;
          } else if (!Device->ReservedBanks[Bank]) {
            Available++;
          }
        }
        DEBUG ((DEBUG_ERROR,
          "Crane SMMU: bank allocation snapshot enabled=%u stream-bound-disabled=%u available=%u architecturally-reserved=%u software-reserved=%u\n",
          Enabled, BoundDisabled, Available, Device->Stage2BankCount + 1U, SoftwareReserved));
      }
      DEBUG_CODE_END ();
    }
    if (Slot->Domain.Failed) {
      /* SmmuAttach may have performed irreversible MMIO writes. */
      Slot->Poisoned = TRUE;
    } else {
      FreeDomainTables (Slot);
      ZeroMem (Slot, sizeof (*Slot));
    }
    DEBUG ((DEBUG_ERROR, "Crane SMMU: AttachStreamLocked stage=hardware-attach status=%r\n", (EFI_STATUS)(Status)));
    return Status;
  }

  if(DisplayAuthorized) Slot->Domain.InheritedStreamOwned=TRUE;
  Slot->InUse   = TRUE;
  Slot->Segment = Segment;
  Slot->Bdf     = Bdf;
  Slot->Sid     = Sid;
  DEBUG ((DEBUG_INIT,
    "Crane SMMU: attached SID=0x%x SMR=%u CB=%u ASID=%u coherent=%u\n",
    Sid, Slot->Domain.Stream, Slot->Domain.Bank, Slot->Domain.Asid,
    Slot->Domain.CacheCoherent));
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: AttachStreamLocked stage=complete status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
  return EFI_SUCCESS;
}

STATIC EFI_STATUS AttachStreamLocked (UINT16 Segment, UINT16 Bdf, UINT16 Sid)
{
  return AttachStreamWithStopLocked (Segment, Bdf, Sid, NULL, NULL, FALSE);
}

typedef struct {
  UINT64 HciBase;
  EFI_SMMU_CR_UFS_VERIFY_STOPPED VerifyStopped;
  VOID *Context;
} CR_SMMU_STOPPED_UFS;

STATIC CR_STATUS VerifyUfsStopped (VOID *Context)
{
  CR_SMMU_STOPPED_UFS *Owner = Context;
  EFI_STATUS Status;
  STATIC CONST UINT16 Offsets[] = {0x24, 0x34, 0x60, 0x80, 0x58, 0x78};
  Status = Owner->VerifyStopped (Owner->Context);
  if (Status != EFI_SUCCESS) return EFI_ERROR(Status) ? (CR_STATUS)Status : CR_NOT_READY;
  /* Clock/reset proof is supplied by the actual native producer. Read its
     independently target-qualified HCI twice; never turn off HCE here. */
  for (UINTN Pass = 0; Pass < 2; Pass++) {
    for (UINTN Index = 0; Index < ARRAY_SIZE (Offsets); Index++) {
      if (MmioRead32 ((UINTN)Owner->HciBase + Offsets[Index]) != 0)
        return CR_BUSY;
    }
    MemoryFence ();
  }
  return CR_SUCCESS;
}

STATIC EFI_STATUS AttachDisplayStreamLocked(UINT16 Sid)
{
  return AttachStreamWithStopLocked(0,CR_SMMU_BDF_UNKNOWN,Sid,NULL,NULL,TRUE);
}
#include "SmmuDisplayGroup.inc.c"

STATIC EFI_STATUS EFIAPI CrSmmuAcquireUfsStream (
    EFI_SMMU_CR_PROTOCOL *This, UINT64 HciBase, UINT16 Sid,
    EFI_SMMU_CR_UFS_VERIFY_STOPPED VerifyStopped, VOID *Context)
{
  EFI_STATUS Status;
  CR_SMMU_STOPPED_UFS Owner = {HciBase, VerifyStopped, Context};
  CONST CrTargetSmmuContext *Target = mSmmu.Target;
  BOOLEAN Allowed = FALSE;
  if (This != &mCrProtocol || !mSmmu.Initialized || mSmmu.AfterExitBootServices ||
      mSmmu.ExistingIoMmu || Target == NULL || VerifyStopped == NULL || Context == NULL)
    return EFI_INVALID_PARAMETER;
  /* Explicit target metadata is separate from SidAllowed. No implicit owner
     can be inferred from BDFffff, a disabled bank, or matching MMIO addresses. */
  if (Target->Base != 0x15000000 || HciBase != 0x1d84000 ||
      HciBase != Target->UfsHciBase || Target->UfsStreamCount == 0 ||
      Target->UfsStreamCount > 2 || Target->UfsStreamIds == NULL)
    return EFI_UNSUPPORTED;
  for (UINTN Index = 0; Index < Target->UfsStreamCount; Index++) {
    if (Target->UfsStreamIds[Index] == Sid) Allowed = TRUE;
  }
  if (!Allowed || !SidAllowed (Sid)) return EFI_ACCESS_DENIED;
  EfiAcquireLock (&mSmmu.Lock);
  Status = AttachStreamWithStopLocked (0, 0xFFFF, Sid, VerifyUfsStopped, &Owner, FALSE);
  EfiReleaseLock (&mSmmu.Lock);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT,
          "Crane SMMU: stopped UFS owner HCI=0x%Lx SID=0x%x status=%r\n", HciBase, Sid, Status));
  return Status;
}

STATIC BOOLEAN UsbTargetValid(CONST CrTargetSmmuContext *T)
{
  if (T->UsbDeviceCount==0) return T->UsbSoc==0 && T->UsbDevices==NULL;
  if (T->Base!=0x15000000 || (T->UsbSoc!=0x8250 && T->UsbSoc!=0x8350) ||
      T->UsbDeviceCount!=2 || T->UsbDevices==NULL ||
      T->DefaultStreamIds==NULL || !T->DefaultStreamCount) return FALSE;
  for (UINTN I=0; I<2; ++I) {
    CONST CrTargetSmmuUsbDevice *D=&T->UsbDevices[I];
    if (D->ControllerBase!=(I?0xa800000ULL:0xa600000ULL) ||
        D->ControllerBytes!=0xcd00 || D->Sid!=(I?0x20:0)) return FALSE;
    BOOLEAN Found=FALSE;
    for (UINTN J=0; J<T->DefaultStreamCount; ++J)
      if (T->DefaultStreamIds[J]==D->Sid) Found=TRUE;
    if (!Found) return FALSE;
  }
  return TRUE;
}
typedef struct {
  CONST CrTargetSmmuUsbDevice *Device;
  EFI_SMMU_CR_USB_VERIFY_STOPPED VerifyStopped;
  VOID *Context;
} CR_SMMU_STOPPED_USB;
STATIC CR_STATUS VerifyUsbStopped(VOID *Context)
{
  CR_SMMU_STOPPED_USB *Owner=Context;
  EFI_STATUS S=Owner->VerifyStopped(Owner->Context);
  if (S!=EFI_SUCCESS) return EFI_ERROR(S)?(CR_STATUS)S:CR_NOT_READY;
  UINTN Base=(UINTN)Owner->Device->ControllerBase;
  /* Public DWC3 core.h offsets and xHCI capability-relative operational
   * registers. Qualify DRD mode before reading function-only registers.
   * Two read passes are observations, never a substitute for writer exclusion. */
  for (UINTN Pass=0; Pass<2; ++Pass) {
    UINT32 Id=MmioRead32(Base+0xc120);
    if (((Id>>16)!=0x5533 && (Id>>16)!=0x3331) ||
        (MmioRead32(Base+0xc140)&3U)!=2) return CR_UNSUPPORTED;
    UINT32 Length=MmioRead32(Base)&0xffU;
    if (Length<0x20 || Length>0x80 || (Length&3U)) return CR_DEVICE_ERROR;
    if (((MmioRead32(Base+0xc110)>>12)&3U)!=1 ||
        (MmioRead32(Base+Length)&(BIT0|BIT2))!=0 ||
        (MmioRead32(Base+Length+4)&BIT0)!=BIT0 ||
        (MmioRead32(Base+0xc704)&BIT31)!=0 ||
        MmioRead32(Base+0xc708)!=0 || MmioRead32(Base+0xc720)!=0 ||
        MmioRead32(Base+0xc400)!=0 || MmioRead32(Base+0xc404)!=0 ||
        MmioRead32(Base+0xc408)!=BIT31 ||
        (MmioRead32(Base+0xc40c)&0xffffU)!=0) return CR_BUSY;
    MemoryFence();
  }
  return CR_SUCCESS;
}
STATIC EFI_STATUS EFIAPI CrSmmuAcquireUsbStream(
    EFI_SMMU_CR_PROTOCOL *This, UINT64 ControllerBase, UINT16 Sid,
    EFI_SMMU_CR_USB_VERIFY_STOPPED VerifyStopped, VOID *Context)
{
  CONST CrTargetSmmuContext *T=mSmmu.Target;
  if (This!=&mCrProtocol || !mSmmu.Initialized || mSmmu.AfterExitBootServices ||
      mSmmu.ExistingIoMmu || T==NULL || VerifyStopped==NULL || Context==NULL)
    return EFI_INVALID_PARAMETER;
  if (!UsbTargetValid(T) || !T->UsbDeviceCount) return EFI_UNSUPPORTED;
  CONST CrTargetSmmuUsbDevice *D=NULL;
  for (UINTN I=0; I<T->UsbDeviceCount; ++I)
    if (T->UsbDevices[I].ControllerBase==ControllerBase) D=&T->UsbDevices[I];
  if (D==NULL) return EFI_UNSUPPORTED;
  if (Sid!=D->Sid || !SidAllowed(Sid)) return EFI_ACCESS_DENIED;
  CR_SMMU_STOPPED_USB Owner={D,VerifyStopped,Context};
  EfiAcquireLock(&mSmmu.Lock);
  CR_SMMU_DOMAIN *Existing=FindDomainBySid(Sid);
  if (Existing!=NULL && (Existing->UsbOwner!=Context || Existing->UsbStopped!=VerifyStopped)) {
    EfiReleaseLock(&mSmmu.Lock);return EFI_ACCESS_DENIED;
  }
  EFI_STATUS S=AttachStreamWithStopLocked(0,0xffff,Sid,VerifyUsbStopped,&Owner,FALSE);
  Existing=FindDomainBySid(Sid);
  if (S==EFI_SUCCESS && Existing!=NULL) {
    Existing->UsbOwner=Context;Existing->UsbStopped=VerifyStopped;
  }
  EfiReleaseLock(&mSmmu.Lock);
  DEBUG((EFI_ERROR(S)?DEBUG_ERROR:DEBUG_INIT,
    "Crane SMMU: stopped USB owner controller=0x%Lx SID=0x%x status=%r\n",ControllerBase,Sid,S));
  return S;
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAttach stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_INIT, "Crane SMMU: attach identity segment=%u BDF=0x%x SID=0x%x external=%u\n", Segment, Bdf, Sid, mSmmu.ExistingIoMmu));
  if (!SidAllowed (Sid)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAttach stage=allow-list status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
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
      DEBUG ((DEBUG_INIT, "Crane SMMU: CrSmmuAttach stage=external-owner status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
      return EFI_SUCCESS;
    }
  }

  EfiAcquireLock (&mSmmu.Lock);
  Status = AttachStreamLocked (Segment, Bdf, Sid);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR,
      "Crane SMMU: attach segment=%u BDF=0x%x SID=0x%x failed: %r\n",
      Segment, Bdf, Sid, Status));
    /* Read only the matching stream and its translated context.  Never
       infer an ownership handoff from these diagnostic register values. */
    DEBUG_CODE_BEGIN ();
    {
      SmmuDevice *Device = &mSmmu.Device;
      UINT16 Index;
      UINT32 Smr;
      UINT32 S2cr;
      UINT32 Mask;
      UINT32 Bank;
      UINT64 Cb;

      if (Device->Io.Read32 != NULL) {
        for (Index = 0; Index < Device->StreamCount; Index++) {
          Smr = Device->Io.Read32 (
            Device->Io.Cookie, Device->Base + 0x800U + 4U * Index);
          Mask = (Smr >> 16) & 0x7FFFU;
          if (((Smr & BIT31) == 0) ||
              (((Sid ^ Smr) & ~Mask & 0xFFFFU) != 0)) {
            continue;
          }
          S2cr = Device->Io.Read32 (
            Device->Io.Cookie, Device->Base + 0xC00U + 4U * Index);
          DEBUG ((DEBUG_ERROR,
            "Crane SMMU: SID=0x%x stream=%u SMR=0x%08x S2CR=0x%08x\n",
            Sid, Index, Smr, S2cr));
          Bank = S2cr & 0xFFU;
          if (((S2cr & 0x30000U) == 0) && (Bank < Device->BankCount)) {
            Cb = Device->ContextBase + (UINT64)Bank * Device->PageSize;
            DEBUG ((DEBUG_ERROR,
              "Crane SMMU: CB=%u SCTLR=0x%08x TCR=0x%08x TCR2=0x%08x "
              "TTBR0=0x%08x%08x TTBR1=0x%08x%08x\n",
              Bank,
              Device->Io.Read32 (Device->Io.Cookie, Cb),
              Device->Io.Read32 (Device->Io.Cookie, Cb + 0x30),
              Device->Io.Read32 (Device->Io.Cookie, Cb + 0x10),
              Device->Io.Read32 (Device->Io.Cookie, Cb + 0x24),
              Device->Io.Read32 (Device->Io.Cookie, Cb + 0x20),
              Device->Io.Read32 (Device->Io.Cookie, Cb + 0x2C),
              Device->Io.Read32 (Device->Io.Cookie, Cb + 0x28)));
          }
        }
      }
    }
    DEBUG_CODE_END ();
  }
  EfiReleaseLock (&mSmmu.Lock);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: CrSmmuAttach stage=complete status=%r\n", (EFI_STATUS)(Status)));
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
  /* BlockSegment belongs to PCI cold-init, never on-chip SID clients.
     Preflight all affected domains before detaching any of the segment. */
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    Slot = &mSmmu.Domains[Index];
    if ((Slot->InUse || Slot->Poisoned) && Slot->Segment == Segment &&
        Slot->Bdf != CR_SMMU_BDF_UNKNOWN && DomainMappingInUse (Index)) {
      EfiReleaseLock (&mSmmu.Lock);
      return EFI_ACCESS_DENIED;
    }
  }

  FirstError = EFI_SUCCESS;
  Found      = FALSE;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
    Slot = &mSmmu.Domains[Index];
    if ((!Slot->InUse && !Slot->Poisoned) || (Slot->Segment != Segment) ||
        (Slot->Bdf == CR_SMMU_BDF_UNKNOWN)) {
      continue;
    }
    Found  = TRUE;
    /* A failed attach may already have published table or stream state.
       Keep its reserved tables and report retained ownership to the host. */
    if (Slot->Poisoned) {
      if (!EFI_ERROR (FirstError)) {
        FirstError = EFI_DEVICE_ERROR;
      }
      continue;
    }
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

  /* A PCI function can own multiple streams. Check every matching domain
     before publishing any WB common-buffer grant; revocation is always legal. */
  if ((Access != 0) && Map->CachedCommonBuffer) {
    for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
      if (mSmmu.Domains[Index].InUse &&
          (mSmmu.Domains[Index].Segment == Segment) &&
          (mSmmu.Domains[Index].Bdf == Bdf) &&
          !mSmmu.Domains[Index].Domain.CacheCoherent) {
        return EFI_UNSUPPORTED;
      }
    }
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
CrSmmuSetAttributeById (
  IN EDKII_IOMMU_PROTOCOL *This, IN UINT64 IommuBase, IN UINT32 DmaId,
  IN VOID *Mapping, IN UINT64 IoMmuAccess
  );

STATIC EFI_STATUS GetNativeDmaIdentity(EFI_HANDLE Handle, UINT16 *Sid)
{
  EFI_DMA_IDENTITY_CR_PROTOCOL *Identity = NULL;
  CR_SMMU_DOMAIN *Domain;
  EFI_STATUS Status;
  UINTN I;
  if (gBS == NULL || gBS->HandleProtocol == NULL || mSmmu.Target == NULL)
    return EFI_NOT_READY;
  Status = gBS->HandleProtocol(Handle, &gEfiDmaIdentityCrProtocolGuid, (VOID **)&Identity);
  if (EFI_ERROR(Status)) return Status;
  if (Identity == NULL || Identity->Revision != EFI_DMA_IDENTITY_CR_PROTOCOL_REVISION ||
      Identity->IommuBase != mSmmu.Device.Base) return EFI_ACCESS_DENIED;
  for (I = 0; I < mSmmu.Target->DmaDeviceCount; ++I) {
    CONST CrTargetSmmuDmaDevice *Device = &mSmmu.Target->DmaDevices[I];
    if (Identity->ControllerBase != Device->Base || Identity->ControllerSize != Device->Size ||
        Identity->Sid != Device->Sid) continue;
    EfiAcquireLock(&mSmmu.Lock);
    Domain = FindDomainBySid(Device->Sid);
    Status = Domain != NULL && Domain->Bdf == 0xFFFF ? EFI_SUCCESS : EFI_NOT_READY;
    EfiReleaseLock(&mSmmu.Lock);
    if (!EFI_ERROR(Status)) *Sid = Device->Sid;
    return Status;
  }
  /* USB stopped acquisition uses its own exact target/journal. Its HCI is
     already running when PCI Map grants DMA, so do not call VerifyStopped
     here. Only the domain committed by AcquireUsbStream may authorize this
     immutable controller tuple; ordinary Attach cannot create that owner. */
  if (UsbTargetValid(mSmmu.Target)) {
    for (I = 0; I < mSmmu.Target->UsbDeviceCount; ++I) {
      CONST CrTargetSmmuUsbDevice *Device = &mSmmu.Target->UsbDevices[I];
      if (Identity->ControllerBase != Device->ControllerBase ||
          Identity->ControllerSize != Device->ControllerBytes ||
          Identity->Sid != Device->Sid) continue;
      EfiAcquireLock(&mSmmu.Lock);
      Domain = FindDomainBySid(Device->Sid);
      Status = Domain != NULL && Domain->Segment == 0 && Domain->Bdf == 0xFFFF &&
               Domain->UsbOwner != NULL && Domain->UsbStopped != NULL
                 ? EFI_SUCCESS : EFI_NOT_READY;
      EfiReleaseLock(&mSmmu.Lock);
      if (!EFI_ERROR(Status)) *Sid = Device->Sid;
      return Status;
    }
  }
  return EFI_ACCESS_DENIED;
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

  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: SetAttribute handle=%p token=%p access=0x%Lx\n", DeviceHandle, Mapping, IoMmuAccess));

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.AfterExitBootServices ||
      ((IoMmuAccess & ~(EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE)) != 0)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttribute stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  if (DeviceHandle == NULL) return EFI_INVALID_PARAMETER;
  Status = GetNativeDmaIdentity(DeviceHandle, &Bdf);
  if (!EFI_ERROR(Status)) return CrSmmuSetAttributeById(This, mSmmu.Device.Base, Bdf, Mapping, IoMmuAccess);
  if (Status != EFI_UNSUPPORTED && Status != EFI_NOT_FOUND) return Status;
  Status = GetPciLocation (DeviceHandle, &Segment, &Bdf);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttribute stage=PCI-location status=%r\n", (EFI_STATUS)(Status)));
    return Status;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Map = FindMapping (Mapping);
  if (Map == NULL) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttribute stage=mapping-token status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: SetAttribute IOVA=0x%Lx PA=0x%Lx pages=%Lu access=0x%Lx\n", Map->IovaBase, Map->PhysicalBase, (UINT64)Map->Pages, IoMmuAccess));
  Access = (UINT32)IoMmuAccess;
  if (!MappingAccessSupported (Map, Access)) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttribute stage=operation-permissions status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
    return EFI_UNSUPPORTED;
  }
  if(DisplayMappingProtected(Map)){EfiReleaseLock(&mSmmu.Lock);return EFI_ACCESS_DENIED;}
  Status = SetMappingAttributeForPciDeviceLocked (Map, Segment, Bdf, Access);
  EfiReleaseLock (&mSmmu.Lock);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: CrSmmuSetAttribute stage=complete status=%r\n", (EFI_STATUS)(Status)));
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

  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: SetAttributeById base=0x%Lx SID=0x%x token=%p access=0x%Lx\n", IommuBase, DmaId, Mapping, IoMmuAccess));

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Mapping == NULL) ||
      ((IoMmuAccess & ~(EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE)) != 0)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttributeById stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  if (IommuBase != mSmmu.Device.Base) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttributeById stage=IOMMU-base status=%r\n", (EFI_STATUS)(EFI_NOT_FOUND)));
    return EFI_NOT_FOUND;
  }
  if (DmaId > 0x7FFFU) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttributeById stage=SID-range status=%r\n", (EFI_STATUS)(EFI_NOT_FOUND)));
    return EFI_NOT_FOUND;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Domain = FindDomainBySid ((UINT16)DmaId);
  Map    = FindMapping (Mapping);
  if(DisplaySid((UINT16)DmaId)||DisplayMappingProtected(Map)){
    EfiReleaseLock(&mSmmu.Lock);return EFI_ACCESS_DENIED;
  }
  if ((Domain == NULL) || (Map == NULL) ||
      !FindDomainIndex (Domain, &DomainIndex)) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttributeById stage=domain-or-mapping status=%r\n", (EFI_STATUS)(EFI_NOT_FOUND)));
    return EFI_NOT_FOUND;
  }
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: SetAttribute IOVA=0x%Lx PA=0x%Lx pages=%Lu access=0x%Lx\n", Map->IovaBase, Map->PhysicalBase, (UINT64)Map->Pages, IoMmuAccess));
  Access = (UINT32)IoMmuAccess;
  if (!MappingAccessSupported (Map, Access)) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuSetAttributeById stage=operation-permissions status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
    return EFI_UNSUPPORTED;
  }
  if ((Access != 0) && Map->CachedCommonBuffer && !Domain->Domain.CacheCoherent) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: cached common buffer requires coherent SID=0x%x\n", DmaId));
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
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: CrSmmuSetAttributeById stage=complete status=%r\n", (EFI_STATUS)(Status)));
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
  CR_SMMU_DMA_BUFFER  *CommonBuffer;
  EFI_STATUS           Status;
  BOOLEAN              CleanupFailed;
  BOOLEAN              UseBounce;

  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: Map request host=%p operation=%u byte-count-ptr=%p\n", HostAddress, Operation, NumberOfBytes));

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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }

  Physical        = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  if ((UINTN)HostAddress > MAX_UINTN - (*NumberOfBytes - 1U)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=host-overflow status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  UseBounce = (Operation == EdkiiIoMmuOperationBusMasterWrite) ||
              (Operation == EdkiiIoMmuOperationBusMasterWrite64);
  if (UseBounce) {
    Offset = 0;
    if (*NumberOfBytes > MAX_UINTN - (SMMU_PAGE_SIZE - 1U)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=bounce-rounding status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
      return EFI_INVALID_PARAMETER;
    }
    Pages = (*NumberOfBytes + SMMU_PAGE_SIZE - 1U) / SMMU_PAGE_SIZE;
    if ((gBS == NULL) || (gBS->AllocatePages == NULL) ||
        (gBS->FreePages == NULL)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=page-services status=%r\n", (EFI_STATUS)(EFI_NOT_READY)));
      return EFI_NOT_READY;
    }
    if (mSmmu.Device.Stage1OutputAddressBits >= 64) {
      Maximum = MAX_UINT64;
    } else if (mSmmu.Device.Stage1OutputAddressBits == 0) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=address-width status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
      return EFI_UNSUPPORTED;
    } else {
      Maximum = (1ULL << mSmmu.Device.Stage1OutputAddressBits) - 1ULL;
    }
    AlignedPhysical = Maximum;
    Status = gBS->AllocatePages (
                    AllocateMaxAddress,
                    EfiBootServicesData,
                    Pages,
                    &AlignedPhysical
                    );
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=bounce-allocation status=%r\n", (EFI_STATUS)(Status)));
      return Status;
    }
    if ((AlignedPhysical == 0) ||
        !AddressRangeFitsDevice (AlignedPhysical, Pages)) {
      (VOID)gBS->FreePages (AlignedPhysical, Pages);
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=bounce-aperture status=%r\n", (EFI_STATUS)(EFI_OUT_OF_RESOURCES)));
      return EFI_OUT_OF_RESOURCES;
    }
    Physical = AlignedPhysical;
  } else {
    Offset          = (UINTN)(Physical & (SMMU_PAGE_SIZE - 1U));
    AlignedPhysical = Physical - Offset;
    if (*NumberOfBytes > MAX_UINTN - Offset) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=host-rounding status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
      return EFI_INVALID_PARAMETER;
    }
    Total = *NumberOfBytes + Offset;
    if (Total > MAX_UINTN - (SMMU_PAGE_SIZE - 1U)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=page-rounding status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
      return EFI_INVALID_PARAMETER;
    }
    Pages = (Total + SMMU_PAGE_SIZE - 1U) / SMMU_PAGE_SIZE;
    if (!AddressRangeFitsDevice (AlignedPhysical, Pages)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=physical-aperture status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
      return EFI_UNSUPPORTED;
    }
  }

  EfiAcquireLock (&mSmmu.Lock);
  if(DisplayAllocationOverlap(AlignedPhysical,(UINT64)Pages*SMMU_PAGE_SIZE)){
    EfiReleaseLock(&mSmmu.Lock);return EFI_ACCESS_DENIED;
  }
  if (!AnyDomainInUse ()) {
    EfiReleaseLock (&mSmmu.Lock);
    if (UseBounce) {
      (VOID)gBS->FreePages (AlignedPhysical, Pages);
    }
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=no-domain status=%r\n", (EFI_STATUS)(EFI_NOT_READY)));
    return EFI_NOT_READY;
  }
  CommonBuffer = NULL;
  if ((Operation == EdkiiIoMmuOperationBusMasterCommonBuffer) ||
      (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer64)) {
    CommonBuffer = FindCommonBufferLocked (AlignedPhysical, Pages);
    if (CommonBuffer == NULL) {
      EfiReleaseLock (&mSmmu.Lock);
      DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=common-buffer-range status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
      return EFI_UNSUPPORTED;
    }
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=map-slot-or-IOVA status=%r\n", (EFI_STATUS)(EFI_OUT_OF_RESOURCES)));
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
  Map->CachedCommonBuffer = (CommonBuffer != NULL) &&
                            ((CommonBuffer->AllocationAttributes & EFI_MEMORY_WB) != 0);
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuMap stage=mapping-rollback status=%r\n", (EFI_STATUS)(Status)));
    return Status;
  }

  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: Map IOVA=0x%Lx PA=0x%Lx pages=%Lu bytes=%Lu operation=%u bounce=%u access=0\n", Map->IovaBase, Map->PhysicalBase, (UINT64)Map->Pages, (UINT64)Map->NumberOfBytes, Operation, UseBounce));
  PrepareMappingForDma (Map);
  *DeviceAddress = Iova;
  *Mapping       = Map;
  EfiReleaseLock (&mSmmu.Lock);
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: CrSmmuMap stage=complete status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuUnmap stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Map = FindMapping (Mapping);
  if (Map == NULL) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuUnmap stage=mapping-token status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  if(DisplayMappingProtected(Map)){EfiReleaseLock(&mSmmu.Lock);return EFI_ACCESS_DENIED;}
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: Unmap IOVA=0x%Lx PA=0x%Lx pages=%Lu\n", Map->IovaBase, Map->PhysicalBase, (UINT64)Map->Pages));
  FirstError = EFI_SUCCESS;
  /* Teardown withdraws the display handoff even if permission revocation
     needs a retry. A partially unmapped surface must not survive exit. */
  Map->PreserveAtExit = FALSE;
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
  DEBUG ((EFI_ERROR (FirstError) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: CrSmmuUnmap stage=complete status=%r\n", (EFI_STATUS)(FirstError)));
  return FirstError;
}

#include "SmmuDisplayBuffer.inc.c"

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
  UINT64                          CacheAttribute;
  UINT64                          AllocationAttributes;
  UINTN                           Index;
  UINTN                           Size;

  (VOID)Type;
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: AllocateBuffer request pages=%Lu type=%u attributes=0x%Lx\n", (UINT64)Pages, MemoryType, Attributes));

  if (HostAddress != NULL) {
    *HostAddress = NULL;
  }
  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Pages == 0) || (Pages > MAX_UINTN / SMMU_PAGE_SIZE) ||
      (HostAddress == NULL) ||
      ((MemoryType != EfiBootServicesData) &&
       (MemoryType != EfiRuntimeServicesData))) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAllocateBuffer stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  if (((Attributes & ~EDKII_IOMMU_ATTRIBUTE_VALID_FOR_ALLOCATE_BUFFER) != 0) ||
      ((Attributes & EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE) != 0)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAllocateBuffer stage=cache-attributes status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
    return EFI_UNSUPPORTED;
  }
  if ((gBS == NULL) || (gBS->AllocatePages == NULL) ||
      (gBS->FreePages == NULL) || (gDS == NULL) ||
      (gDS->GetMemorySpaceDescriptor == NULL) ||
      (gDS->SetMemorySpaceAttributes == NULL)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAllocateBuffer stage=services status=%r\n", (EFI_STATUS)(EFI_NOT_READY)));
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAllocateBuffer stage=buffer-slots status=%r\n", (EFI_STATUS)(EFI_OUT_OF_RESOURCES)));
    return EFI_OUT_OF_RESOURCES;
  }

  Address = (mSmmu.Device.Stage1OutputAddressBits >= 64)
              ? MAX_UINT64
              : ((mSmmu.Device.Stage1OutputAddressBits == 0)
                   ? 0
                   : ((1ULL << mSmmu.Device.Stage1OutputAddressBits) - 1ULL));
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
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: AllocateBuffer AllocatePages PA=0x%Lx pages=%Lu status=%r\n", Address, (UINT64)Pages, Status));
  if (EFI_ERROR (Status)) {
    goto ReleaseSlot;
  }
  if (!AddressRangeFitsDevice (Address, Pages)) {
    Status = EFI_OUT_OF_RESOURCES;
    goto FreePages;
  }

  Status = gDS->GetMemorySpaceDescriptor (Address, &Descriptor);
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: AllocateBuffer GCD descriptor PA=0x%Lx status=%r\n", Address, Status));
  if (EFI_ERROR (Status)) {
    goto FreePages;
  }
  /*
   * On AArch64 EFI_MEMORY_UC is mapped as Device-nGnRnE by CpuDxe.  These
   * pages need normal loads/stores. Default clients use Normal-NC (WC).
   * Explicit MEMORY_CACHED requests use WB, matching Android's coherent
   * dma_pgprot/PAGE_KERNEL allocation. WB common-buffer grants are admitted
   * only to coherent domains; there is no NC fallback for a cached request.
   */
  CacheAttribute = (Attributes & EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) != 0
                     ? EFI_MEMORY_WB : EFI_MEMORY_WC;
  if ((Descriptor.Capabilities & CacheAttribute) == 0) {
    Status = EFI_UNSUPPORTED;
    goto FreePages;
  }

  Size = Pages * SMMU_PAGE_SIZE;
  AllocationAttributes =
    (Descriptor.Attributes & ~EFI_CACHE_ATTRIBUTE_MASK) | CacheAttribute;
  WriteBackInvalidateDataCacheRange ((VOID *)(UINTN)Address, Size);
  Status = gDS->SetMemorySpaceAttributes (
                  Address,
                  Size,
                  AllocationAttributes
                  );
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: AllocateBuffer set-cache=0x%Lx PA=0x%Lx pages=%Lu status=%r\n", CacheAttribute, Address, (UINT64)Pages, Status));
  if (EFI_ERROR (Status)) {
    goto FreePages;
  }

  EfiAcquireLock (&mSmmu.Lock);
  Slot->Address            = Address;
  Slot->Pages              = Pages;
  Slot->OriginalAttributes = Descriptor.Attributes;
  Slot->AllocationAttributes = AllocationAttributes;
  Slot->InUse              = TRUE;
  Slot->Busy               = FALSE;
  EfiReleaseLock (&mSmmu.Lock);
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: AllocateBuffer PA=0x%Lx pages=%Lu memory-type=%u attributes=0x%Lx\n", Address, (UINT64)Pages, MemoryType, Attributes));
  *HostAddress = (VOID *)(UINTN)Address;
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: CrSmmuAllocateBuffer stage=complete status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
  return EFI_SUCCESS;

FreePages:
  (VOID)gBS->FreePages (Address, Pages);
ReleaseSlot:
  EfiAcquireLock (&mSmmu.Lock);
  ZeroMem (Slot, sizeof (*Slot));
  EfiReleaseLock (&mSmmu.Lock);
  DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuAllocateBuffer stage=allocation-cleanup status=%r\n", (EFI_STATUS)(Status)));
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
  BOOLEAN              KeepBusy;
  UINTN                Index;
  UINTN                Size;

  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: FreeBuffer request host=%p pages=%Lu\n", HostAddress, (UINT64)Pages));

  if ((This != &mIoMmuProtocol) || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices ||
      (Pages == 0) || (Pages > MAX_UINTN / SMMU_PAGE_SIZE) ||
      (HostAddress == NULL) ||
      (((UINTN)HostAddress & (SMMU_PAGE_SIZE - 1U)) != 0)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuFreeBuffer stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  if ((gBS == NULL) || (gBS->FreePages == NULL) || (gDS == NULL) ||
      (gDS->SetMemorySpaceAttributes == NULL)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuFreeBuffer stage=services status=%r\n", (EFI_STATUS)(EFI_NOT_READY)));
    return EFI_NOT_READY;
  }

  Address = (EFI_PHYSICAL_ADDRESS)(UINTN)HostAddress;
  Slot    = NULL;
  EfiAcquireLock (&mSmmu.Lock);
  if (DisplayAllocationOverlap(Address, (UINT64)Pages * SMMU_PAGE_SIZE) ||
      BufferMappingInUse (Address, (UINT64)Pages * SMMU_PAGE_SIZE)) {
    EfiReleaseLock (&mSmmu.Lock);
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuFreeBuffer stage=live-mapping status=%r\n", (EFI_STATUS)(EFI_ACCESS_DENIED)));
    return EFI_ACCESS_DENIED;
  }
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: CrSmmuFreeBuffer stage=buffer-record status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }

  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: FreeBuffer PA=0x%Lx pages=%Lu\n", Address, (UINT64)Pages));
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
      /* Retained ownership must retain the actual WB/NC allocation policy,
         including its protection bits, so a later map/retry stays compatible. */
      RestoreStatus = gDS->SetMemorySpaceAttributes (
                            Address,
                            Size,
                            Slot->AllocationAttributes
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
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_VERBOSE, "Crane SMMU: CrSmmuFreeBuffer stage=complete status=%r\n", (EFI_STATUS)(Status)));
  return Status;
}

STATIC
EFI_STATUS
EFIAPI
CrSmmuPreserveReadOnlyMapping (
  IN EFI_SMMU_CR_PROTOCOL *This,
  IN VOID                *Mapping
  )
{
  CR_SMMU_MAPPING                 *Map;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR Descriptor;
  EFI_STATUS                      Status;
  UINT64                          Bytes;
  UINT64                          Offset;

  if (This != &mCrProtocol || !mSmmu.Initialized ||
      mSmmu.ExistingIoMmu || mSmmu.AfterExitBootServices || Mapping == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (gDS == NULL || gDS->GetMemorySpaceDescriptor == NULL) {
    return EFI_NOT_READY;
  }
  EfiAcquireLock (&mSmmu.Lock);
  Map = FindMapping (Mapping);
  if (Map == NULL) {
    Status = EFI_INVALID_PARAMETER;
    goto Done;
  }
  if(DisplayMappingProtected(Map)){Status=EFI_ACCESS_DENIED;goto Done;}
  if ((Map->Operation != EdkiiIoMmuOperationBusMasterRead &&
       Map->Operation != EdkiiIoMmuOperationBusMasterRead64) ||
      Map->BounceAddress != 0 || Map->DeviceWriteGranted ||
      Map->Pages == 0 || Map->Pages > MAX_UINT64 / SMMU_PAGE_SIZE) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }
  Bytes = (UINT64)Map->Pages * SMMU_PAGE_SIZE;
  Status = gDS->GetMemorySpaceDescriptor (Map->PhysicalBase, &Descriptor);
  if (EFI_ERROR (Status)) goto Done;
  /* A partial final page is also accessible to DMA. Require every mapped
     page, not just the requested pixel bytes, to stay reserved after exit. */
  if (Descriptor.GcdMemoryType != EfiGcdMemoryTypeReserved ||
      Descriptor.BaseAddress > Map->PhysicalBase ||
      Bytes - 1 > MAX_UINT64 - Map->PhysicalBase) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }
  Offset = Map->PhysicalBase - Descriptor.BaseAddress;
  if (Offset >= Descriptor.Length || Bytes > Descriptor.Length - Offset) {
    Status = EFI_UNSUPPORTED;
    goto Done;
  }
  Map->PreserveAtExit = TRUE;
  Status = EFI_SUCCESS;
Done:
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
CrSmmuSetExitObserver (EFI_SMMU_CR_PROTOCOL *This, VOID *Owner,
                     EFI_SMMU_CR_EXIT_OBSERVER Callback, VOID *Context)
{
  UINTN Index, Free = ARRAY_SIZE(mSmmu.ExitObservers);
  EFI_STATUS Status = EFI_OUT_OF_RESOURCES;
  if (This != &mCrProtocol || Owner == NULL) return EFI_INVALID_PARAMETER;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu) return EFI_NOT_READY;
  EfiAcquireLock (&mSmmu.Lock);
  if (mSmmu.AfterExitBootServices) { Status = EFI_ACCESS_DENIED; goto Done; }
  for (Index = 0; Index < ARRAY_SIZE(mSmmu.ExitObservers); ++Index) {
    if (mSmmu.ExitObservers[Index].Owner == Owner) {
      if (Callback == NULL) {
        ZeroMem (&mSmmu.ExitObservers[Index], sizeof(mSmmu.ExitObservers[Index]));
        Status = EFI_SUCCESS;
      } else {
        Status = mSmmu.ExitObservers[Index].Callback == Callback &&
                 mSmmu.ExitObservers[Index].Context == Context ? EFI_SUCCESS : EFI_ALREADY_STARTED;
      }
      goto Done;
    }
    if (mSmmu.ExitObservers[Index].Owner == NULL) Free = Index;
  }
  if (Callback == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  if (Free < ARRAY_SIZE(mSmmu.ExitObservers)) {
    mSmmu.ExitObservers[Free].Owner = Owner;
    mSmmu.ExitObservers[Free].Callback = Callback;
    mSmmu.ExitObservers[Free].Context = Context;
    Status = EFI_SUCCESS;
  }
Done:
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
CrSmmuDumpFault (EFI_SMMU_CR_PROTOCOL *This, UINT16 Segment, UINT16 Bdf, UINT16 Sid)
{
  CR_SMMU_DOMAIN *Domain;
  UINTN DomainIndex, Index;
  EFI_STATUS Status;
  if (This != &mCrProtocol) return EFI_INVALID_PARAMETER;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu) return EFI_NOT_READY;
  EfiAcquireLock (&mSmmu.Lock);
  if (mSmmu.AfterExitBootServices) { Status = EFI_ACCESS_DENIED; goto Done; }
  Domain = FindDomainBySid (Sid);
  if (Domain == NULL || Domain->Segment != Segment || Domain->Bdf != Bdf ||
      !FindDomainIndex (Domain, &DomainIndex)) { Status = EFI_NOT_FOUND; goto Done; }
  Status = (EFI_STATUS)SmmuDumpFaultState (&Domain->Domain);
  if (Status != EFI_SUCCESS) goto Done;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    CR_SMMU_MAPPING *Map = &mSmmu.Mappings[Index];
    if (!Map->InUse || Map->DomainAccess[DomainIndex] == 0) continue;
    DEBUG ((DEBUG_ERROR, "Crane SMMU: fault mapping SID=0x%x id=%u op=%u IOVA=0x%Lx PA=0x%Lx host=0x%Lx bytes=%Lu pages=%Lu access=%x\n",
      Sid, (UINT32)Index + 1, Map->Operation, Map->IovaBase, Map->PhysicalBase,
      Map->HostAddress, (UINT64)Map->NumberOfBytes, (UINT64)Map->Pages,
      Map->DomainAccess[DomainIndex]));
    if (Map->IovaBase >= SMMU_IOVA_BASE && Map->Pages != 0 &&
        Map->IovaBase - SMMU_IOVA_BASE < SMMU_IOVA_SIZE &&
        Map->Pages <= (SMMU_IOVA_SIZE - (Map->IovaBase - SMMU_IOVA_BASE)) / SMMU_PAGE_SIZE) {
      UINTN First = (UINTN)((Map->IovaBase - SMMU_IOVA_BASE) / SMMU_PAGE_SIZE);
      DEBUG ((DEBUG_ERROR, "Crane SMMU: fault mapping id=%u L1=%Lx L2=%Lx first-PTE=%Lx last-PTE=%Lx\n",
        (UINT32)Index + 1, Domain->Domain.Tables[0],
        Domain->Domain.Tables[512 + (Map->IovaBase >> 21)],
        Domain->Domain.Tables[1024 + First],
        Domain->Domain.Tables[1024 + First + Map->Pages - 1]));
    }
  }
Done:
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
CrSmmuSetExitPrepareObserver (EFI_SMMU_CR_PROTOCOL *This, VOID *Owner,
                     EFI_SMMU_CR_EXIT_PREPARE Callback, VOID *Context)
{
  UINTN Index, Free = ARRAY_SIZE(mSmmu.ExitPrepareObservers);
  EFI_STATUS Status = EFI_OUT_OF_RESOURCES;
  if (This != &mCrProtocol || Owner == NULL) return EFI_INVALID_PARAMETER;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu) return EFI_NOT_READY;
  EfiAcquireLock (&mSmmu.Lock);
  if (mSmmu.AfterExitBootServices) { Status = EFI_ACCESS_DENIED; goto Done; }
  for (Index = 0; Index < ARRAY_SIZE(mSmmu.ExitPrepareObservers); ++Index) {
    if (mSmmu.ExitPrepareObservers[Index].Owner == Owner) {
      if (Callback == NULL) {
        ZeroMem (&mSmmu.ExitPrepareObservers[Index], sizeof(mSmmu.ExitPrepareObservers[Index]));
        Status = EFI_SUCCESS;
      } else {
        Status = mSmmu.ExitPrepareObservers[Index].Callback == Callback &&
                 mSmmu.ExitPrepareObservers[Index].Context == Context ? EFI_SUCCESS : EFI_ALREADY_STARTED;
      }
      goto Done;
    }
    if (mSmmu.ExitPrepareObservers[Index].Owner == NULL) Free = Index;
  }
  if (Callback == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  if (Free < ARRAY_SIZE(mSmmu.ExitPrepareObservers)) {
    mSmmu.ExitPrepareObservers[Free].Owner = Owner;
    mSmmu.ExitPrepareObservers[Free].Callback = Callback;
    mSmmu.ExitPrepareObservers[Free].Context = Context;
    Status = EFI_SUCCESS;
  }
Done:
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS
QueryExitMapping (EFI_SMMU_CR_PROTOCOL *This, VOID *Mapping,
                  EFI_SMMU_CR_MAPPING_EVIDENCE *Evidence, BOOLEAN Revoked)
{
  EFI_SMMU_CR_MAPPING_EVIDENCE Result = {0};
  CR_SMMU_MAPPING *Map;
  CR_SMMU_DOMAIN *Domain;
  EFI_STATUS Status = EFI_NOT_READY;
  UINTN Index;
  if (Evidence == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem (Evidence, sizeof(*Evidence));
  if (This != &mCrProtocol || Mapping == NULL) return EFI_INVALID_PARAMETER;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu) return EFI_NOT_READY;
  EfiAcquireLock (&mSmmu.Lock);
  if (!mSmmu.ExitComplete) goto Done;
  Map = FindMapping (Mapping);
  if (Map == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  if ((!Revoked && !Map->PreserveAtExit) || Map->BounceAddress != 0 || Map->DeviceWriteGranted ||
      (Map->Operation != EdkiiIoMmuOperationBusMasterRead &&
       Map->Operation != EdkiiIoMmuOperationBusMasterRead64) ||
      Map->Pages == 0 || Map->Pages > MAX_UINT64 / SMMU_PAGE_SIZE) {
    Status = EFI_ACCESS_DENIED; goto Done;
  }
  for (Index = 0; Index < ARRAY_SIZE(mSmmu.Domains); ++Index) {
    UINT32 Access = Revoked ? Map->ExitDomainAccess[Index] : Map->DomainAccess[Index];
    if (Access == 0) continue;
    Domain = &mSmmu.Domains[Index];
    if (!Domain->InUse || Domain->Poisoned || (!Revoked && !Domain->PreservedAtExit) ||
        Access != EDKII_IOMMU_ACCESS_READ ||
        Result.StreamCount >= ARRAY_SIZE(Result.Streams)) {
      Status = EFI_ACCESS_DENIED; goto Done;
    }
    Status = Revoked ? (EFI_STATUS)SmmuQueryRevokedMapping (&Domain->Domain,
               Map->IovaBase, Map->Pages, &Result.Streams[Result.StreamCount]) :
             (EFI_STATUS)SmmuQueryMappingEvidence (&Domain->Domain,
               Map->IovaBase, Map->PhysicalBase, Map->Pages, SMMU_ACCESS_READ,
               &Result.Streams[Result.StreamCount]);
    if (EFI_ERROR(Status)) goto Done;
    Result.StreamCount++;
  }
  if (Result.StreamCount == 0) { Status = EFI_NOT_FOUND; goto Done; }
  Result.Size = sizeof(Result); Result.Revision = 1;
  Result.MappingId = (UINT32)(Map - mSmmu.Mappings) + 1;
  Result.HostAddress = Map->HostAddress; Result.NumberOfBytes = Map->NumberOfBytes;
  Result.PhysicalBase = Map->PhysicalBase; Result.IovaBase = Map->IovaBase;
  Result.PageBytes = (UINT64)Map->Pages * SMMU_PAGE_SIZE;
  Result.DeviceAddress = Map->IovaBase + Map->HostAddress - Map->PhysicalBase;
  CopyMem (Evidence, &Result, sizeof(Result));
  Status = EFI_SUCCESS;
Done:
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
CrSmmuQueryExitMapping (EFI_SMMU_CR_PROTOCOL *This, VOID *Mapping,
                       EFI_SMMU_CR_MAPPING_EVIDENCE *Evidence) {
  return QueryExitMapping(This, Mapping, Evidence, FALSE);
}
STATIC EFI_STATUS EFIAPI
CrSmmuQueryExitRevokedMapping (EFI_SMMU_CR_PROTOCOL *This, VOID *Mapping,
                              EFI_SMMU_CR_MAPPING_EVIDENCE *Evidence) {
  return QueryExitMapping(This, Mapping, Evidence, TRUE);
}

STATIC EFI_STATUS EFIAPI
CrSmmuQueryExitRevokedDomain (EFI_SMMU_CR_PROTOCOL *This, UINT16 Segment,
                             UINT16 Bdf, UINT16 Sid, SmmuMappingEvidence *Evidence)
{
  EFI_STATUS Status = EFI_NOT_READY;
  SmmuMappingEvidence Result = {0};
  if (Evidence == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Evidence, sizeof(*Evidence));
  if (This != &mCrProtocol) return EFI_INVALID_PARAMETER;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu) return EFI_NOT_READY;
  EfiAcquireLock(&mSmmu.Lock);
  if (!mSmmu.ExitComplete) goto Done;
  CR_SMMU_DOMAIN *Domain = FindDomainBySid(Sid);
  if (Domain == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  if (Domain->Segment != Segment || Domain->Bdf != Bdf || Domain->Poisoned || Domain->PreservedAtExit) {
    Status = EFI_ACCESS_DENIED; goto Done;
  }
  Status = (EFI_STATUS)SmmuQueryRevokedMapping(&Domain->Domain, SMMU_IOVA_BASE,
                       SMMU_IOVA_SIZE / SMMU_PAGE_SIZE, &Result);
  if (Status == EFI_SUCCESS) CopyMem(Evidence, &Result, sizeof(Result));
  else if (!EFI_ERROR(Status)) Status = EFI_NOT_READY;
Done:
  EfiReleaseLock(&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
CrSmmuReleaseUsbToPhysicalAtExit (EFI_SMMU_CR_PROTOCOL *This)
{
  EFI_STATUS Status = EFI_ACCESS_DENIED;
  if (This != &mCrProtocol) return EFI_INVALID_PARAMETER;
  if (!FixedPcdGetBool(PcdUsbLegacyPhysicalDmaHandoff)) return EFI_UNSUPPORTED;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu ||
      mSmmu.Device.Base != 0x15000000ULL) return EFI_UNSUPPORTED;
  EfiAcquireLock(&mSmmu.Lock);
  if (!mSmmu.AfterExitBootServices || !mSmmu.ExitComplete ||
      !mSmmu.ExitObserversActive) goto Done;
  CR_SMMU_DOMAIN *Domain = FindDomainBySid(0);
  if (Domain == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  if (!Domain->InUse || Domain->Segment != 0 || Domain->Bdf != 0xffff ||
      Domain->Poisoned || Domain->PreservedAtExit) goto Done;
  // The portable transition independently checks live stream/context identity,
  // complete revocation and exclusive CB ownership before its first write.
  Status = (EFI_STATUS)SmmuReleaseToPhysicalAtExit(&Domain->Domain);
  if (Status != EFI_SUCCESS && !EFI_ERROR(Status)) Status = EFI_NOT_READY;
  DEBUG((EFI_ERROR(Status)?DEBUG_ERROR:DEBUG_INIT,
    "Crane SMMU: legacy USB physical handoff SID=0 CB=%u status=%r\n",
    Domain->Domain.Bank,Status));
Done:
  EfiReleaseLock(&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI
CrSmmuReleaseUfsToCoherentPhysicalAtExit (EFI_SMMU_CR_PROTOCOL *This)
{
  EFI_STATUS Status = EFI_ACCESS_DENIED;
  CONST UINTN Hci = 0x01D84000;
  if (This != &mCrProtocol) return EFI_INVALID_PARAMETER;
  if (!FixedPcdGetBool (PcdUfsLegacyCoherentDmaHandoff)) return EFI_UNSUPPORTED;
  if (!mSmmu.Initialized || mSmmu.ExistingIoMmu ||
      mSmmu.Device.Base != 0x15000000ULL) return EFI_UNSUPPORTED;
  EfiAcquireLock (&mSmmu.Lock);
  if (!mSmmu.AfterExitBootServices || !mSmmu.ExitComplete ||
      !mSmmu.ExitObserversActive) goto Done;
  CR_SMMU_DOMAIN *Domain = FindDomainBySid (0xE0);
  if (Domain == NULL) { Status = EFI_NOT_FOUND; goto Done; }
  if (!Domain->InUse || Domain->Segment != 0 || Domain->Bdf != 0xffff ||
      Domain->Poisoned || Domain->PreservedAtExit) goto Done;
  // Independently refuse a running or inaccessible controller. The UFS owner
  // has already sealed software admission before its prepare callback stops it.
  if (MmioRead32 (Hci + 0x24) != 0 || (MmioRead32 (Hci + 0x34) & BIT0) != 0 ||
      (MmioRead32 (Hci + 0x60) & BIT0) != 0 || (MmioRead32 (Hci + 0x80) & BIT0) != 0 ||
      MmioRead32 (Hci + 0x58) != 0 || MmioRead32 (Hci + 0x78) != 0) goto Done;
  Status = (EFI_STATUS)SmmuReleaseToCoherentPhysicalAtExit (&Domain->Domain);
  if (Status != EFI_SUCCESS && !EFI_ERROR (Status)) Status = EFI_NOT_READY;
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT,
    "Crane SMMU: legacy UFS coherent handoff SID=0xE0 CB=%u status=%r\n",
    Domain->Domain.Bank, Status));
Done:
  EfiReleaseLock (&mSmmu.Lock);
  return Status;
}

STATIC EFI_STATUS EFIAPI CrSmmuQueryInheritedDisplay(
  EFI_SMMU_CR_PROTOCOL *This, UINT64 Device, UINT64 Bytes,
  CR_SMMU_DISPLAY_TABLE_READ TableRead, VOID *Context,
  CR_SMMU_DISPLAY_OBSERVATION *Observation)
{
  EFI_STATUS Status = EFI_UNSUPPORTED;
  if (Observation == NULL) return EFI_INVALID_PARAMETER;
  ZeroMem(Observation, sizeof(*Observation));
  if (This != &mCrProtocol) return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mSmmu.Lock);
  if (!mSmmu.Initialized || mSmmu.AfterExitBootServices || mSmmu.ExistingIoMmu ||
      mSmmu.Target == NULL || mSmmu.Target->DisplayReservedBytes == 0) goto Done;
  for (UINTN I=0; I<mSmmu.DomainLimit; ++I) {
    if (mSmmu.Domains[I].InUse &&
        (mSmmu.Domains[I].Sid & ~0x402U) == 0x820U) {
      Status = EFI_ACCESS_DENIED; goto Done;
    }
  }
  Status = (EFI_STATUS)CrSmmuDisplayObserve(&mSmmu.Device, Device, Bytes,
      mSmmu.Target->DisplayReservedBase, mSmmu.Target->DisplayReservedBytes,
      TableRead, Context, Observation);
Done:
  EfiReleaseLock(&mSmmu.Lock);
  return Status;
}

/* Keep the opted-in framebuffer translation, never an entire DMA domain.
   Page tables are allocated as EfiReservedMemoryType and remain live until
   the OS replaces the firmware SMMU context. No allocation at exit is needed. */
STATIC
BOOLEAN
PreserveDomainAtExitLocked (
  IN UINTN DomainIndex
  )
{
  CR_SMMU_DOMAIN  *Domain = &mSmmu.Domains[DomainIndex];
  CR_SMMU_MAPPING *Map;
  UINTN           Index;
  EFI_STATUS      Status;
  BOOLEAN         Preserve = FALSE;

  if (Domain->Poisoned || Domain->Domain.Failed || !Domain->Domain.Active) {
    return FALSE;
  }
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    Map = &mSmmu.Mappings[Index];
    if (Map->InUse && Map->PreserveAtExit &&
        Map->DomainAccess[DomainIndex] == EDKII_IOMMU_ACCESS_READ) {
      Preserve = TRUE;
      break;
    }
  }
  if (!Preserve) return FALSE;
  for (Index = 0; Index < ARRAY_SIZE (mSmmu.Mappings); Index++) {
    Map = &mSmmu.Mappings[Index];
    if (!Map->InUse || Map->DomainAccess[DomainIndex] == 0) continue;
    if (Map->PreserveAtExit &&
        Map->DomainAccess[DomainIndex] == EDKII_IOMMU_ACCESS_READ) continue;
    Status = (EFI_STATUS)SmmuSetMapping (&Domain->Domain, Map->IovaBase, 0,
                                        Map->Pages, 0);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "Crane SMMU: SID 0x%x handoff revoke failed: %r\n",
              Domain->Sid, Status));
      return FALSE;
    }
    Map->DomainAccess[DomainIndex] = 0;
  }
  return TRUE;
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
  if (mSmmu.AfterExitBootServices) return;
  DEBUG ((DEBUG_INIT, "Crane SMMU: EBS initialized=%u external=%u\n", mSmmu.Initialized, mSmmu.ExistingIoMmu));
  mSmmu.AfterExitBootServices = TRUE;
  mSmmu.ExitStatus = mSmmu.Initialized && !mSmmu.ExistingIoMmu ? EFI_SUCCESS : EFI_NOT_READY;
  mSmmu.ExitPreparing = TRUE;
  for (Index = 0; Index < ARRAY_SIZE(mSmmu.ExitPrepareObservers); ++Index) {
    if (mSmmu.ExitPrepareObservers[Index].Callback != NULL) {
      Status = mSmmu.ExitPrepareObservers[Index].Callback(mSmmu.ExitPrepareObservers[Index].Context);
      if (Status != EFI_SUCCESS && mSmmu.ExitStatus == EFI_SUCCESS)
        mSmmu.ExitStatus = EFI_ERROR(Status) ? Status : EFI_NOT_READY;
    }
  }
  mSmmu.ExitPreparing = FALSE;
  if (mSmmu.Initialized && !mSmmu.ExistingIoMmu) {
    EfiAcquireLock (&mSmmu.Lock);
    /* Snapshot actual grants before the revocation pass clears its ledger. */
    for (Index = 0; Index < ARRAY_SIZE(mSmmu.Mappings); ++Index) {
      if (mSmmu.Mappings[Index].InUse)
        CopyMem(mSmmu.Mappings[Index].ExitDomainAccess,
                mSmmu.Mappings[Index].DomainAccess,
                sizeof(mSmmu.Mappings[Index].ExitDomainAccess));
    }
    for (Index = 0; Index < ARRAY_SIZE (mSmmu.Domains); Index++) {
      if (!mSmmu.Domains[Index].InUse && !mSmmu.Domains[Index].Poisoned) {
        continue;
      }
      if (PreserveDomainAtExitLocked (Index)) {
        mSmmu.Domains[Index].PreservedAtExit = TRUE;
        DEBUG ((DEBUG_INIT, "Crane SMMU: EBS preserved SID=0x%x CB=%u table=0x%Lx\n", mSmmu.Domains[Index].Sid, mSmmu.Domains[Index].Domain.Bank, mSmmu.Domains[Index].TableAddress));
        continue;
      }
      Status = (EFI_STATUS)SmmuBlock (&mSmmu.Domains[Index].Domain);
      DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT, "Crane SMMU: EBS block SID=0x%x CB=%u poisoned=%u table=0x%Lx status=%r\n", mSmmu.Domains[Index].Sid, mSmmu.Domains[Index].Domain.Bank, mSmmu.Domains[Index].Poisoned, mSmmu.Domains[Index].TableAddress, Status));
      if (EFI_ERROR (Status)) {
        if (!EFI_ERROR(mSmmu.ExitStatus)) mSmmu.ExitStatus = Status;
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
  mSmmu.ExitComplete = TRUE;
  mSmmu.ExitObserversActive = TRUE;
  for (Index = 0; Index < ARRAY_SIZE(mSmmu.ExitObservers); ++Index) {
    if (mSmmu.ExitObservers[Index].Callback != NULL)
      mSmmu.ExitObservers[Index].Callback(mSmmu.ExitObservers[Index].Context, mSmmu.ExitStatus);
  }
  mSmmu.ExitObserversActive = FALSE;
}

STATIC
EFI_STATUS
GetTargetSmmuContext (
  OUT CONST CrTargetSmmuContext **Target
  )
{
  CONST CrTargetSmmuContext *Context;
  UINTN Index;
  UINTN Other;
  BOOLEAN Found;

  if (Target == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  Context = CrDalGetSmmuContext ();
  if ((Context == NULL) || (Context->Base == 0) || (Context->Size == 0) ||
      (Context->SegmentCount == 0) ||
      (Context->DefaultStreamCount != 0 &&
       Context->DefaultStreamIds == NULL) ||
      (Context->ActlrSettingCount != 0 && Context->ActlrSettings == NULL) ||
      (Context->DmaDeviceCount != 0 && Context->DmaDevices == NULL) ||
      (Context->CoherentStreamCount != 0 &&
       Context->CoherentStreamIds == NULL)) {
    return EFI_INVALID_PARAMETER;
  }
  if (Context->DefaultStreamCount > CR_SMMU_MAX_DOMAINS) {
    return EFI_OUT_OF_RESOURCES;
  }
  if (Context->CoherentStreamCount > Context->DefaultStreamCount) {
    return EFI_INVALID_PARAMETER;
  }
  if (!UsbTargetValid(Context)) return EFI_INVALID_PARAMETER;
  if (Context->UfsStreamCount != 0) {
    if (Context->Base != 0x15000000 || Context->UfsHciBase != 0x1d84000 ||
        Context->UfsStreamIds == NULL || Context->UfsStreamCount > 2 ||
        Context->UfsStreamIds[0] != 0xE0 ||
        (Context->UfsStreamCount == 2 && Context->UfsStreamIds[1] != 0x4E0))
      return EFI_INVALID_PARAMETER;
    for (Index = 0; Index < Context->UfsStreamCount; Index++) {
      Found = FALSE;
      for (Other = 0; Other < Context->DefaultStreamCount; Other++)
        if (Context->UfsStreamIds[Index] == Context->DefaultStreamIds[Other]) Found = TRUE;
      if (!Found) return EFI_INVALID_PARAMETER;
    }
  } else if (Context->UfsHciBase != 0 || Context->UfsStreamIds != NULL) {
    return EFI_INVALID_PARAMETER;
  }
  for (Index = 0; Index < Context->CoherentStreamCount; Index++) {
    Found = FALSE;
    for (Other = 0; Other < Context->DefaultStreamCount; Other++) {
      if (Context->CoherentStreamIds[Index] == Context->DefaultStreamIds[Other]) {
        Found = TRUE;
        break;
      }
    }
    if (!Found) {
      return EFI_INVALID_PARAMETER;
    }
    for (Other = 0; Other < Index; Other++) {
      if (Context->CoherentStreamIds[Index] == Context->CoherentStreamIds[Other]) {
        return EFI_INVALID_PARAMETER;
      }
    }
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
  DEBUG ((DEBUG_INIT, "Crane SMMU: target base=0x%Lx size=0x%Lx segments=%u allow-SIDs=%u domain-limit=%Lu external-owns=%u ACTLR-rules=%u\n", Target->Base, (UINT64)Target->Size, Target->SegmentCount, Target->DefaultStreamCount, (UINT64)mSmmu.DomainLimit, Target->ExternalIoMmuOwnsPcieStreams, Target->ActlrSettingCount));
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: InitializeExternalSmmuState stage=arguments status=%r\n", (EFI_STATUS)(EFI_INVALID_PARAMETER)));
    return EFI_INVALID_PARAMETER;
  }
  Status = GetTargetSmmuContext (&Target);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: InitializeExternalSmmuState stage=target status=%r\n", (EFI_STATUS)(Status)));
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: InitializeExternalSmmuState stage=ownership-policy status=%r\n", (EFI_STATUS)(EFI_UNSUPPORTED)));
    return EFI_UNSUPPORTED;
  }

  /* The external owner handles all MMIO programming in this mode. */
  mSmmu.Device.Base       = Target->Base;
  /* Keep metadata coherent for defensive helper paths even though this
     adapter refuses to submit mappings while an external producer is active. */
  mSmmu.Device.InputAddressBits         = 64;
  mSmmu.Device.Stage1OutputAddressBits  = 64;
  mSmmu.Device.AddressBits = 64;
  mSmmu.Initialized       = TRUE;
  DEBUG ((DEBUG_INIT, "Crane SMMU: using existing IoMmu producer %p\n", Existing));
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: InitializeExternalSmmuState stage=complete status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
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
    DEBUG ((DEBUG_ERROR, "Crane SMMU: InitializeSmmuState stage=target status=%r\n", (EFI_STATUS)(Status)));
    return Status;
  }
  InitializeSmmuMetadata (Target);

  CrStatus = SmmuProbe (&mSmmu.Device, Target->Base, Target->Size, &mSmmuIoOps);
  if (CR_ERROR (CrStatus)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: InitializeSmmuState stage=probe status=%r\n", (EFI_STATUS)((EFI_STATUS)CrStatus)));
    return (EFI_STATUS)CrStatus;
  }
  DEBUG ((DEBUG_INIT, "Crane SMMU: probe success base=0x%Lx CB-base=0x%Lx SMRs=%u CBs=%u stage2=%u page-size=%u UBS-bits=%u UBS-enc=%u IAS-bits=%u IAS-enc=%u OAS-bits=%u OAS-enc=%u\n", mSmmu.Device.Base, mSmmu.Device.ContextBase, mSmmu.Device.StreamCount, mSmmu.Device.BankCount, mSmmu.Device.Stage2BankCount, mSmmu.Device.PageSize, mSmmu.Device.InputAddressBits, mSmmu.Device.InputAddressEncoding, mSmmu.Device.Stage1OutputAddressBits, mSmmu.Device.Stage1OutputAddressEncoding, mSmmu.Device.AddressBits, mSmmu.Device.AddressEncoding));
  mSmmu.Device.ActlrSettings = Target->ActlrSettings;
  mSmmu.Device.ActlrSettingCount = Target->ActlrSettingCount;
  mSmmu.Device.SctlrReadbackIgnoreMask = Target->SctlrReadbackIgnoreMask;
  mSmmu.Initialized = TRUE;

  /* Stream ownership is attached by the PCIe resource callback.  The target
     list is an allow-list only; binding all SIDs here would lose the real
     segment/BDF association for multi-root platforms. */
  DEBUG ((DEBUG_VERBOSE, "Crane SMMU: InitializeSmmuState stage=complete status=%r\n", (EFI_STATUS)(EFI_SUCCESS)));
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
  DEBUG ((DEBUG_INIT, "Crane SMMU: driver entry image=%p\n", ImageHandle));

  /* A standard IOMMU protocol producer may already own the controller. */
  Existing = NULL;
  Status   = gBS->LocateProtocol (&gEdkiiIoMmuProtocolGuid, NULL,
                                  (VOID **)&Existing);
  DEBUG ((DEBUG_INIT, "Crane SMMU: LocateProtocol external=%p status=%r\n", Existing, Status));
  if (!EFI_ERROR (Status) && (Existing != NULL)) {
    Status = InitializeExternalSmmuState (Existing);
  } else {
    Status = InitializeSmmuState ();
  }
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: initialization failed: %r\n", Status));
    DEBUG ((DEBUG_ERROR, "Crane SMMU: SmmuCrDxeEntryPoint stage=initialize status=%r\n", (EFI_STATUS)(Status)));
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
  mCrProtocol.PreserveReadOnlyMapping = CrSmmuPreserveReadOnlyMapping;
  mCrProtocol.QueryExitMapping = CrSmmuQueryExitMapping;
  mCrProtocol.SetExitObserver = CrSmmuSetExitObserver;
  mCrProtocol.QueryExitRevokedMapping = CrSmmuQueryExitRevokedMapping;
  mCrProtocol.SetExitPrepareObserver = CrSmmuSetExitPrepareObserver;
  mCrProtocol.QueryExitRevokedDomain = CrSmmuQueryExitRevokedDomain;
  mCrProtocol.ReleaseUsbToPhysicalAtExit = CrSmmuReleaseUsbToPhysicalAtExit;
  mCrProtocol.ReleaseUfsToCoherentPhysicalAtExit = CrSmmuReleaseUfsToCoherentPhysicalAtExit;
  mCrProtocol.DumpFault = CrSmmuDumpFault;
  mCrProtocol.AcquireUfsStream = CrSmmuAcquireUfsStream;
  mCrProtocol.QueryInheritedDisplay = CrSmmuQueryInheritedDisplay;
  mCrProtocol.AcquireUsbStream = CrSmmuAcquireUsbStream;
  mCrProtocol.AcquireDisplayGroup = CrSmmuAcquireDisplayGroup;
  mCrProtocol.BindDisplayFrame = CrSmmuBindDisplayFrame;
  mCrProtocol.AllocateDisplayCommand = CrSmmuAllocateDisplayCommand;
  mCrProtocol.PrepareDisplayBuffer = CrSmmuPrepareDisplayBuffer;
  mCrProtocol.CheckDisplayBuffer = CrSmmuCheckDisplayBuffer;
  mCrProtocol.QueryDisplayBuffer = CrSmmuQueryDisplayBuffer;
  mCrProtocol.PreserveDisplayFrame = CrSmmuPreserveDisplayFrame;
  mCrProtocol.ReleaseDisplayBuffer = CrSmmuReleaseDisplayBuffer;

  Status = gBS->CreateEventEx (
                  EVT_NOTIFY_SIGNAL,
                  TPL_CALLBACK,
                  CrSmmuExitBootServices,
                  NULL,
                  &gEfiEventExitBootServicesGuid,
                  &mSmmu.ExitBootServicesEvent
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "Crane SMMU: SmmuCrDxeEntryPoint stage=create-EBS-event status=%r\n", (EFI_STATUS)(Status)));
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
  DEBUG ((EFI_ERROR (Status) ? DEBUG_ERROR : DEBUG_INIT, "Crane SMMU: SmmuCrDxeEntryPoint stage=install-protocols status=%r\n", (EFI_STATUS)(Status)));
  return Status;
}
