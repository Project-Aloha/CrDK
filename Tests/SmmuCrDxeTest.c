/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include "../Driver/SmmuCrDxe/SmmuCrDxe.c"

#define TEST_PAGE_COUNT 256U
#define TEST_MAX_CALLS  96U

typedef struct {
  SmmuDomain *Domain;
  UINT64      Iova;
  UINT64      Physical;
  UINTN       Pages;
  UINT32      Access;
} SET_CALL;

STATIC EFI_BOOT_SERVICES mBootServices;
STATIC EFI_DXE_SERVICES  mDxeServices;
EFI_BOOT_SERVICES       *gBS = &mBootServices;
EFI_DXE_SERVICES        *gDS = &mDxeServices;

EFI_GUID gEfiPciIoProtocolGuid = EFI_PCI_IO_PROTOCOL_GUID;

UINT32 EFIAPI
MmioRead32 (
  IN UINTN Address
  )
{
  (VOID)Address;
  return 0;
}

UINT32 EFIAPI
MmioWrite32 (
  IN UINTN  Address,
  IN UINT32 Value
  )
{
  (VOID)Address;
  return Value;
}

UINT64 EFIAPI
MmioWrite64 (
  IN UINTN  Address,
  IN UINT64 Value
  )
{
  (VOID)Address;
  return Value;
}

UINTN EFIAPI
MicroSecondDelay (
  IN UINTN MicroSeconds
  )
{
  return MicroSeconds;
}

STATIC UINT8 mPagePool[TEST_PAGE_COUNT * SMMU_PAGE_SIZE]
  __attribute__((aligned(SMMU_PAGE_SIZE)));
STATIC UINTN                mNextPage;
STATIC UINTN                mAllocateCalls;
STATIC UINTN                mFreeCalls;
STATIC EFI_MEMORY_TYPE      mLastMemoryType;
STATIC EFI_STATUS           mAllocateStatus;
STATIC EFI_STATUS           mFreeStatus;
STATIC UINTN                mCloseCalls;
STATIC SET_CALL             mSetLog[TEST_MAX_CALLS];
STATIC UINTN                mSetCalls;
STATIC UINTN                mFailSetCall1;
STATIC UINTN                mFailSetCall2;
STATIC UINTN                mBlockCalls;
STATIC UINTN                mFailBlockCall;
STATIC UINTN                mDetachCalls;
STATIC UINTN                mAttachCalls;
STATIC UINTN                mWriteBackCalls;
STATIC UINTN                mWriteBackInvalidateCalls;
STATIC UINTN                mInvalidateCalls;
STATIC VOID                *mLastCacheAddress;
STATIC UINTN                mLastCacheLength;
STATIC UINTN                mGetDescriptorCalls;
STATIC UINTN                mSetMemoryAttributeCalls;
STATIC UINT64               mLastMemoryAttributes;
STATIC EFI_STATUS           mSetMemoryAttributeStatus;
STATIC EFI_STATUS           mGetDescriptorStatus;

STATIC EFI_HANDLE CONST mHandleSegment0 = (EFI_HANDLE)(UINTN)0x1000;
STATIC EFI_HANDLE CONST mHandleSegment1 = (EFI_HANDLE)(UINTN)0x1001;
STATIC EFI_HANDLE CONST mHandleRoot      = (EFI_HANDLE)(UINTN)0x1002;
STATIC EFI_HANDLE CONST mHandleBad       = (EFI_HANDLE)(UINTN)0x1003;

STATIC EFI_STATUS EFIAPI
MockGetLocation (
  IN EFI_PCI_IO_PROTOCOL *This,
  OUT UINTN              *Segment,
  OUT UINTN              *Bus,
  OUT UINTN              *Device,
  OUT UINTN              *Function
  );

STATIC EFI_PCI_IO_PROTOCOL mPciSegment0 = {
  .GetLocation = MockGetLocation
};
STATIC EFI_PCI_IO_PROTOCOL mPciSegment1 = {
  .GetLocation = MockGetLocation
};
STATIC EFI_PCI_IO_PROTOCOL mPciRoot = {
  .GetLocation = MockGetLocation
};
STATIC EFI_PCI_IO_PROTOCOL mPciBad = {
  .GetLocation = MockGetLocation
};

STATIC CONST CrTargetSmmuContext mTarget = {
  .SegmentCount   = 2,
  .MaximumDomains = CR_SMMU_MAX_DOMAINS
};

EFI_LOCK *EFIAPI
EfiInitializeLock (
  IN OUT EFI_LOCK *Lock,
  IN EFI_TPL       Priority
  )
{
  Lock->Tpl      = Priority;
  Lock->OwnerTpl = TPL_APPLICATION;
  Lock->Lock     = EfiLockReleased;
  return Lock;
}

VOID EFIAPI
EfiAcquireLock (
  IN EFI_LOCK *Lock
  )
{
  assert (Lock != NULL && Lock->Lock == EfiLockReleased);
  Lock->Lock = EfiLockAcquired;
}

VOID EFIAPI
EfiReleaseLock (
  IN EFI_LOCK *Lock
  )
{
  assert (Lock != NULL && Lock->Lock == EfiLockAcquired);
  Lock->Lock = EfiLockReleased;
}

VOID *EFIAPI
CopyMem (
  OUT VOID       *Destination,
  IN CONST VOID  *Source,
  IN UINTN        Length
  )
{
  return memcpy (Destination, Source, Length);
}

VOID *EFIAPI
ZeroMem (
  OUT VOID *Buffer,
  IN UINTN  Length
  )
{
  return memset (Buffer, 0, Length);
}

VOID *EFIAPI
WriteBackDataCacheRange (
  IN VOID  *Address,
  IN UINTN  Length
  )
{
  mWriteBackCalls++;
  mLastCacheAddress = Address;
  mLastCacheLength  = Length;
  return Address;
}

VOID *EFIAPI
WriteBackInvalidateDataCacheRange (
  IN VOID  *Address,
  IN UINTN  Length
  )
{
  mWriteBackInvalidateCalls++;
  mLastCacheAddress = Address;
  mLastCacheLength  = Length;
  return Address;
}

VOID *EFIAPI
InvalidateDataCacheRange (
  IN VOID  *Address,
  IN UINTN  Length
  )
{
  mInvalidateCalls++;
  mLastCacheAddress = Address;
  mLastCacheLength  = Length;
  return Address;
}

STATIC EFI_STATUS EFIAPI
MockAllocatePages (
  IN EFI_ALLOCATE_TYPE     Type,
  IN EFI_MEMORY_TYPE       MemoryType,
  IN UINTN                 Pages,
  IN OUT EFI_PHYSICAL_ADDRESS *Memory
  )
{
  (VOID)Type;
  assert (Memory != NULL);
  mAllocateCalls++;
  mLastMemoryType = MemoryType;
  if (EFI_ERROR (mAllocateStatus)) {
    return mAllocateStatus;
  }
  if ((Pages == 0) || (Pages > TEST_PAGE_COUNT - mNextPage)) {
    return EFI_OUT_OF_RESOURCES;
  }
  *Memory = (EFI_PHYSICAL_ADDRESS)(UINTN)&mPagePool[mNextPage * SMMU_PAGE_SIZE];
  mNextPage += Pages;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockFreePages (
  IN EFI_PHYSICAL_ADDRESS Memory,
  IN UINTN                Pages
  )
{
  assert (Memory != 0 && Pages != 0);
  mFreeCalls++;
  return mFreeStatus;
}

STATIC EFI_STATUS EFIAPI
MockCloseEvent (
  IN EFI_EVENT Event
  )
{
  assert (Event != NULL);
  mCloseCalls++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockHandleProtocol (
  IN EFI_HANDLE Handle,
  IN EFI_GUID  *Protocol,
  OUT VOID    **Interface
  )
{
  assert (Protocol == &gEfiPciIoProtocolGuid && Interface != NULL);
  if (Handle == mHandleSegment0) {
    *Interface = &mPciSegment0;
  } else if (Handle == mHandleSegment1) {
    *Interface = &mPciSegment1;
  } else if (Handle == mHandleRoot) {
    *Interface = &mPciRoot;
  } else if (Handle == mHandleBad) {
    *Interface = &mPciBad;
  } else {
    *Interface = NULL;
    return EFI_UNSUPPORTED;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockGetLocation (
  IN EFI_PCI_IO_PROTOCOL *This,
  OUT UINTN              *Segment,
  OUT UINTN              *Bus,
  OUT UINTN              *Device,
  OUT UINTN              *Function
  )
{
  assert (Segment != NULL && Bus != NULL && Device != NULL && Function != NULL);
  if (This == &mPciSegment0) {
    *Segment = 0;
    *Bus     = 1;
    *Device  = 0;
    *Function = 0;
  } else if (This == &mPciSegment1) {
    *Segment = 1;
    *Bus     = 1;
    *Device  = 0;
    *Function = 0;
  } else if (This == &mPciRoot) {
    *Segment = 0;
    *Bus     = 0;
    *Device  = 0;
    *Function = 0;
  } else {
    *Segment = 0;
    *Bus     = 0x100;
    *Device  = 0;
    *Function = 0;
  }
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockGetMemorySpaceDescriptor (
  IN EFI_PHYSICAL_ADDRESS            Address,
  OUT EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor
  )
{
  assert (Address != 0 && Descriptor != NULL);
  mGetDescriptorCalls++;
  if (EFI_ERROR (mGetDescriptorStatus)) {
    return mGetDescriptorStatus;
  }
  ZeroMem (Descriptor, sizeof (*Descriptor));
  Descriptor->BaseAddress  = (EFI_PHYSICAL_ADDRESS)(UINTN)mPagePool;
  Descriptor->Length       = sizeof (mPagePool);
  Descriptor->Capabilities = EFI_MEMORY_UC | EFI_MEMORY_WB | EFI_MEMORY_XP;
  Descriptor->Attributes   = EFI_MEMORY_WB | EFI_MEMORY_XP;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockSetMemorySpaceAttributes (
  IN EFI_PHYSICAL_ADDRESS Address,
  IN UINT64               Length,
  IN UINT64               Attributes
  )
{
  assert (Address != 0 && Length != 0);
  mSetMemoryAttributeCalls++;
  mLastMemoryAttributes = Attributes;
  return mSetMemoryAttributeStatus;
}

CR_STATUS
SmmuSetMapping (
  SmmuDomain *Domain,
  UINT64      Iova,
  UINT64      Physical,
  UINTN       Pages,
  UINT32      Access
  )
{
  assert (mSetCalls < ARRAY_SIZE (mSetLog));
  mSetLog[mSetCalls].Domain   = Domain;
  mSetLog[mSetCalls].Iova     = Iova;
  mSetLog[mSetCalls].Physical = Physical;
  mSetLog[mSetCalls].Pages    = Pages;
  mSetLog[mSetCalls].Access   = Access;
  mSetCalls++;
  if ((mSetCalls == mFailSetCall1) || (mSetCalls == mFailSetCall2)) {
    return CR_DEVICE_ERROR;
  }
  return CR_SUCCESS;
}

CR_STATUS
SmmuBlock (
  SmmuDomain *Domain
  )
{
  assert (Domain != NULL);
  mBlockCalls++;
  return (mBlockCalls == mFailBlockCall) ? CR_DEVICE_ERROR : CR_SUCCESS;
}

CR_STATUS
SmmuDetach (
  SmmuDomain *Domain
  )
{
  assert (Domain != NULL);
  mDetachCalls++;
  Domain->Active = FALSE;
  return CR_SUCCESS;
}

CR_STATUS
SmmuAttach (
  SmmuDevice *Device,
  SmmuDomain *Domain,
  UINT16      Sid,
  VOID       *Tables,
  UINT64      TableAddress
  )
{
  assert (Device == &mSmmu.Device && Domain != NULL && Sid <= 0x7FFFU);
  assert (Tables != NULL && TableAddress == (UINT64)(UINTN)Tables);
  mAttachCalls++;
  Domain->Device  = Device;
  Domain->Tables  = Tables;
  Domain->Active  = TRUE;
  Domain->Sid     = Sid;
  return CR_SUCCESS;
}

STATIC VOID
ResetCalls (
  VOID
  )
{
  ZeroMem (mSetLog, sizeof (mSetLog));
  mSetCalls                    = 0;
  mFailSetCall1                = 0;
  mFailSetCall2                = 0;
  mBlockCalls                  = 0;
  mFailBlockCall               = 0;
  mDetachCalls                 = 0;
  mAttachCalls                 = 0;
  mWriteBackCalls              = 0;
  mWriteBackInvalidateCalls    = 0;
  mInvalidateCalls             = 0;
  mLastCacheAddress            = NULL;
  mLastCacheLength             = 0;
}

STATIC VOID
ResetState (
  VOID
  )
{
  ZeroMem (&mSmmu, sizeof (mSmmu));
  ZeroMem (&mBootServices, sizeof (mBootServices));
  ZeroMem (&mDxeServices, sizeof (mDxeServices));
  ZeroMem (mPagePool, sizeof (mPagePool));
  mBootServices.AllocatePages = MockAllocatePages;
  mBootServices.FreePages     = MockFreePages;
  mBootServices.HandleProtocol = MockHandleProtocol;
  mBootServices.CloseEvent     = MockCloseEvent;
  mDxeServices.GetMemorySpaceDescriptor = MockGetMemorySpaceDescriptor;
  mDxeServices.SetMemorySpaceAttributes = MockSetMemorySpaceAttributes;
  mSmmu.Initialized        = TRUE;
  mSmmu.Device.AddressBits = 64;
  mSmmu.Device.Base        = 0x15000000U;
  mSmmu.DomainLimit        = CR_SMMU_MAX_DOMAINS;
  mSmmu.Target             = &mTarget;
  EfiInitializeLock (&mSmmu.Lock, TPL_NOTIFY);
  mNextPage                = 0;
  mAllocateCalls           = 0;
  mFreeCalls               = 0;
  mLastMemoryType          = EfiMaxMemoryType;
  mAllocateStatus          = EFI_SUCCESS;
  mFreeStatus              = EFI_SUCCESS;
  mCloseCalls              = 0;
  mGetDescriptorCalls      = 0;
  mSetMemoryAttributeCalls = 0;
  mLastMemoryAttributes    = 0;
  mSetMemoryAttributeStatus = EFI_SUCCESS;
  mGetDescriptorStatus      = EFI_SUCCESS;
  ResetCalls ();
}

STATIC VOID
AddDomain (
  IN UINTN  Index,
  IN UINT16 Segment,
  IN UINT16 Bdf,
  IN UINT16 Sid
  )
{
  assert (Index < ARRAY_SIZE (mSmmu.Domains));
  mSmmu.Domains[Index].InUse        = TRUE;
  mSmmu.Domains[Index].Segment      = Segment;
  mSmmu.Domains[Index].Bdf          = Bdf;
  mSmmu.Domains[Index].Sid          = Sid;
  mSmmu.Domains[Index].Domain.Device = &mSmmu.Device;
  mSmmu.Domains[Index].Domain.Active = TRUE;
}

STATIC VOID *
MapReadBuffer (
  IN VOID  *Buffer,
  IN UINTN  Size
  )
{
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  VOID                *Mapping;

  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterRead,
            Buffer,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_SUCCESS);
  assert (Mapping != NULL && DeviceAddress >= SMMU_IOVA_BASE);
  return Mapping;
}

STATIC VOID
TestPerDeviceIsolation (
  VOID
  )
{
  STATIC UINT8 Buffer[SMMU_PAGE_SIZE] __attribute__((aligned(SMMU_PAGE_SIZE)));
  CR_SMMU_MAPPING *Map;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  AddDomain (1, 1, 0x0100, 0x1C81);
  AddDomain (2, 0, 0x0000, 0x1C00);
  Map = MapReadBuffer (Buffer, sizeof (Buffer));

  ResetCalls ();
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleSegment0,
            Map,
            EDKII_IOMMU_ACCESS_READ
            ) == EFI_SUCCESS);
  assert (mSetCalls == 1 && mSetLog[0].Domain == &mSmmu.Domains[0].Domain);
  assert (Map->DomainAccess[0] == EDKII_IOMMU_ACCESS_READ);
  assert (Map->DomainAccess[1] == 0 && Map->DomainAccess[2] == 0);

  ResetCalls ();
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleSegment1,
            Map,
            EDKII_IOMMU_ACCESS_READ
            ) == EFI_SUCCESS);
  assert (mSetCalls == 1 && mSetLog[0].Domain == &mSmmu.Domains[1].Domain);
  assert (Map->DomainAccess[1] == EDKII_IOMMU_ACCESS_READ);

  ResetCalls ();
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleSegment1,
            Map,
            EDKII_IOMMU_ACCESS_WRITE
            ) == EFI_UNSUPPORTED);
  assert (mSetCalls == 0);

  ResetCalls ();
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            (EFI_HANDLE)(UINTN)0xDEAD,
            Map,
            EDKII_IOMMU_ACCESS_READ
            ) == EFI_UNSUPPORTED);
  assert (mSetCalls == 0);
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleBad,
            Map,
            EDKII_IOMMU_ACCESS_READ
            ) == EFI_UNSUPPORTED);
  assert (mSetCalls == 0);

  assert (CrSmmuSetAttributeById (
            &mIoMmuProtocol,
            0,
            0x1C01,
            Map,
            EDKII_IOMMU_ACCESS_READ
            ) == EFI_NOT_FOUND);
  assert (CrSmmuSetAttributeById (
            &mIoMmuProtocol,
            mSmmu.Device.Base,
            0x1C00,
            Map,
            EDKII_IOMMU_ACCESS_READ
            ) == EFI_SUCCESS);
  assert (mSetCalls == 1 && mSetLog[0].Domain == &mSmmu.Domains[2].Domain);

  ResetCalls ();
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
}

STATIC VOID
TestPermissionRollback (
  VOID
  )
{
  UINT8                Buffer[64];
  CR_SMMU_MAPPING *Map;
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  VOID                *Mapping;
  UINTN                Size;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  AddDomain (1, 0, 0x0100, 0x1C02);
  Size = sizeof (Buffer);
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterWrite,
            Buffer,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_SUCCESS);
  Map = Mapping;

  ResetCalls ();
  mFailSetCall1 = 2;
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleSegment0,
            Map,
            EDKII_IOMMU_ACCESS_WRITE
            ) == EFI_DEVICE_ERROR);
  assert (mSetCalls == 3);
  assert (mSetLog[2].Domain == &mSmmu.Domains[0].Domain &&
          mSetLog[2].Access == 0);
  assert (Map->DomainAccess[0] == 0 && Map->DomainAccess[1] == 0);
  assert (!Map->DeviceWriteGranted);

  ResetCalls ();
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
}

STATIC VOID
TestFailedMapQuarantinesBounce (
  VOID
  )
{
  UINT8                Buffer[64];
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  VOID                *Mapping;
  UINTN                Size;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  AddDomain (1, 1, 0x0100, 0x1C81);
  mFailSetCall1 = 2;
  mFailSetCall2 = 3;
  DeviceAddress = 0xBAD;
  Mapping       = (VOID *)(UINTN)0xBAD;
  Size          = sizeof (Buffer);
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterWrite,
            Buffer,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_DEVICE_ERROR);
  assert (DeviceAddress == 0 && Mapping == NULL);
  assert (mSmmu.Mappings[0].InUse &&
          mSmmu.Mappings[0].BounceAddress != 0 &&
          BitmapIsSet (0));
  assert (mFreeCalls == 0);
}

STATIC VOID
TestMapBoundsAndUnmapRetry (
  VOID
  )
{
  STATIC UINT8 Buffer[SMMU_PAGE_SIZE] __attribute__((aligned(SMMU_PAGE_SIZE)));
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  VOID                *Mapping;
  UINTN                Size;
  CR_SMMU_MAPPING     *Map;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  DeviceAddress = 0xBAD;
  Mapping       = (VOID *)(UINTN)0xBAD;
  Size          = sizeof (Buffer);
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            (EDKII_IOMMU_OPERATION)-1,
            Buffer,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_INVALID_PARAMETER);
  assert (DeviceAddress == 0 && Mapping == NULL);

  mSmmu.Device.AddressBits = 32;
  DeviceAddress = 0xBAD;
  Mapping       = (VOID *)(UINTN)0xBAD;
  Size          = 0x1000;
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterRead,
            (VOID *)(UINTN)0xFFFFF800U,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_UNSUPPORTED);
  assert (DeviceAddress == 0 && Mapping == NULL && !BitmapIsSet (0));

  mSmmu.Device.AddressBits = 64;
  Size = sizeof (Buffer);
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterCommonBuffer,
            Buffer,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_UNSUPPORTED);
  assert (Mapping == NULL && !BitmapIsSet (0));

  AddDomain (1, 1, 0x0100, 0x1C81);
  AddDomain (2, 0, 0x0000, 0x1C00);
  Map = MapReadBuffer (Buffer, sizeof (Buffer));
  ResetCalls ();
  mFailSetCall1 = 2;
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_DEVICE_ERROR);
  assert (mSetCalls == 3 && Map->InUse && BitmapIsSet (0));
  ResetCalls ();
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
  assert (!BitmapIsSet (0));
}

STATIC VOID
TestWriteBounceAndFailedAuthorization (
  VOID
  )
{
  UINT8                Neighbor[96];
  UINT8                Original[sizeof (Neighbor)];
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  CR_SMMU_MAPPING     *Map;
  VOID                *Mapping;
  UINTN                Size;
  UINTN                Index;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  for (Index = 0; Index < sizeof (Neighbor); Index++) {
    Neighbor[Index] = (UINT8)Index;
  }
  CopyMem (Original, Neighbor, sizeof (Neighbor));
  Size = 37;
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterWrite,
            &Neighbor[17],
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_SUCCESS);
  Map = Mapping;
  assert (Map->BounceAddress != 0 && Map->BouncePages == 1);
  assert (DeviceAddress == Map->IovaBase);
  assert (memcmp ((VOID *)(UINTN)Map->BounceAddress, &Neighbor[17], Size) == 0);
  assert (mWriteBackInvalidateCalls == 1);

  ResetCalls ();
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleSegment0,
            Map,
            EDKII_IOMMU_ACCESS_WRITE
            ) == EFI_SUCCESS);
  memset ((VOID *)(UINTN)Map->BounceAddress, 0xA5, Size);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
  assert (mInvalidateCalls == 1);
  assert (memcmp (Neighbor, Original, 17) == 0);
  for (Index = 17; Index < 17 + Size; Index++) {
    assert (Neighbor[Index] == 0xA5);
  }
  assert (memcmp (&Neighbor[17 + Size], &Original[17 + Size],
                  sizeof (Neighbor) - 17 - Size) == 0);

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  for (Index = 0; Index < sizeof (Neighbor); Index++) {
    Neighbor[Index] = (UINT8)(0x80U + Index);
  }
  CopyMem (Original, Neighbor, sizeof (Neighbor));
  Size = 37;
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterWrite,
            &Neighbor[17],
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_SUCCESS);
  Map = Mapping;
  ResetCalls ();
  mFailSetCall1 = 1;
  assert (CrSmmuSetAttribute (
            &mIoMmuProtocol,
            mHandleSegment0,
            Map,
            EDKII_IOMMU_ACCESS_WRITE
            ) == EFI_DEVICE_ERROR);
  memset ((VOID *)(UINTN)Map->BounceAddress, 0xCC, Size);
  ResetCalls ();
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
  assert (mInvalidateCalls == 0);
  assert (memcmp (Neighbor, Original, sizeof (Neighbor)) == 0);
}

STATIC VOID
TestCommonBufferAllocation (
  VOID
  )
{
  VOID                *HostAddress;
  VOID                *Mapping;
  EFI_PHYSICAL_ADDRESS DeviceAddress;
  UINTN                Size;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  HostAddress = (VOID *)(UINTN)0xBAD;
  assert (CrSmmuAllocateBuffer (
            &mIoMmuProtocol,
            AllocateAnyPages,
            EfiBootServicesCode,
            1,
            &HostAddress,
            0
            ) == EFI_INVALID_PARAMETER);
  assert (HostAddress == NULL);
  assert (CrSmmuAllocateBuffer (
            &mIoMmuProtocol,
            AllocateAnyPages,
            EfiBootServicesData,
            1,
            &HostAddress,
            EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED
            ) == EFI_UNSUPPORTED);
  assert (HostAddress == NULL);

  assert (CrSmmuAllocateBuffer (
            &mIoMmuProtocol,
            AllocateAnyPages,
            EfiBootServicesData,
            2,
            &HostAddress,
            0
            ) == EFI_SUCCESS);
  assert (HostAddress != NULL && mLastMemoryType == EfiBootServicesData);
  assert (mGetDescriptorCalls == 1 && mSetMemoryAttributeCalls == 1);
  assert ((mLastMemoryAttributes & EFI_MEMORY_UC) != 0);
  assert ((mLastMemoryAttributes & EFI_MEMORY_WB) == 0);

  Size = 2 * SMMU_PAGE_SIZE;
  assert (CrSmmuMap (
            &mIoMmuProtocol,
            EdkiiIoMmuOperationBusMasterCommonBuffer,
            HostAddress,
            &Size,
            &DeviceAddress,
            &Mapping
            ) == EFI_SUCCESS);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Mapping) == EFI_SUCCESS);
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 1, HostAddress) ==
          EFI_INVALID_PARAMETER);
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 2, HostAddress) == EFI_SUCCESS);
  assert (mFreeCalls == 1);
  assert (mLastMemoryAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP));
}

STATIC VOID
TestAttachBlockAndExit (
  VOID
  )
{
  EFI_STATUS Status;

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  mSmmu.Mappings[0].InUse = TRUE;
  assert (AttachStreamLocked (0, 0x0100, 0x1C01) == EFI_SUCCESS);
  assert (AttachStreamLocked (1, 0x0100, 0x1C01) == EFI_ACCESS_DENIED);
  assert (AttachStreamLocked (0, 0x0000, 0x1C01) == EFI_ACCESS_DENIED);

  ResetState ();
  Status = AttachStreamLocked (0, 0x0100, 0x1C01);
  assert (Status == EFI_SUCCESS && mAttachCalls == 1);
  assert (mLastMemoryType == EfiReservedMemoryType);

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  AddDomain (1, 0, 0x0000, 0x1C00);
  AddDomain (2, 1, 0x0100, 0x1C81);
  mFailBlockCall = 1;
  assert (CrSmmuBlockSegment (&mCrProtocol, 0) == EFI_DEVICE_ERROR);
  assert (mBlockCalls == 2 && mDetachCalls == 1);
  assert (mSmmu.Domains[0].InUse && !mSmmu.Domains[1].InUse);
  assert (mSmmu.Domains[2].InUse);

  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  AddDomain (1, 0, 0x0000, 0x1C00);
  AddDomain (2, 1, 0x0100, 0x1C81);
  mFailBlockCall = 2;
  mSmmu.ExitBootServicesEvent = (EFI_EVENT)(UINTN)0xCAFE;
  CrSmmuExitBootServices (mSmmu.ExitBootServicesEvent, NULL);
  assert (mSmmu.AfterExitBootServices && mBlockCalls == 3);
  assert (mSmmu.Domains[0].InUse && mSmmu.Domains[1].InUse &&
          mSmmu.Domains[2].InUse);
  assert (mDetachCalls == 0 && mFreeCalls == 0 && mCloseCalls == 1);
  assert (mSmmu.ExitBootServicesEvent == NULL);
}

int
main (
  void
  )
{
  TestPerDeviceIsolation ();
  TestPermissionRollback ();
  TestFailedMapQuarantinesBounce ();
  TestMapBoundsAndUnmapRetry ();
  TestWriteBounceAndFailedAuthorization ();
  TestCommonBufferAllocation ();
  TestAttachBlockAndExit ();
  puts ("Crane SMMU adapter: isolation, bounce, allocation and EBS checks passed");
  return 0;
}
