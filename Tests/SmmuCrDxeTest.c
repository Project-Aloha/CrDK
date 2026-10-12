/* SPDX-License-Identifier: MIT */
#ifndef _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff
#define _PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff FALSE
#endif
#ifndef _PCD_VALUE_PcdUsbLegacyPhysicalDmaHandoff
#define _PCD_VALUE_PcdUsbLegacyPhysicalDmaHandoff FALSE
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>

#undef NULL
#include "../Driver/SmmuCrDxe/SmmuCrDxe.c"

/* Exercise the shipped target separately from the synthetic policy fixtures. */
#define CrTargetGetSmmuContext CrTargetGetActualSm8450SmmuContext
#include "../Target/Sm8450/CrSmmuTarget.c"
#undef CrTargetGetSmmuContext

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
EFI_GUID gEfiDmaIdentityCrProtocolGuid = EFI_DMA_IDENTITY_CR_PROTOCOL_GUID;
STATIC EFI_DMA_IDENTITY_CR_PROTOCOL *mIdentity;

STATIC UINTN mDiagnosticCount;
STATIC BOOLEAN mSawMapDiagnostic, mSawEbsDiagnostic, mSawAttachDiagnostic;
BOOLEAN EFIAPI DebugPrintEnabled (VOID) { return TRUE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled (UINTN Level) { (VOID)Level; return TRUE; }
BOOLEAN EFIAPI DebugAssertEnabled (VOID) { return TRUE; }
VOID EFIAPI MemoryFence (VOID) { }
VOID EFIAPI DebugAssert (CONST CHAR8 *File, UINTN Line, CONST CHAR8 *Description)
{ fprintf (stderr, "%s:%zu: %s\n", File, (size_t)Line, Description); assert (0); }
VOID EFIAPI DebugPrint (UINTN Level, CONST CHAR8 *Format, ...)
{
#ifndef CR_SMMU_REAL_CORE
  assert (Level == DEBUG_ERROR || Level == DEBUG_INIT || Level == DEBUG_VERBOSE);
  assert (Format != NULL && strstr (Format, "Crane SMMU:") != NULL);
#else
  (VOID)Level; assert (Format != NULL);
#endif
  mDiagnosticCount++;
  if (strstr (Format, "Map IOVA=") != NULL) mSawMapDiagnostic = TRUE;
  if (strstr (Format, "EBS block SID=") != NULL) mSawEbsDiagnostic = TRUE;
  if (strstr (Format, "attach identity") != NULL) mSawAttachDiagnostic = TRUE;
}

BOOLEAN EFIAPI
DebugCodeEnabled (VOID)
{
  return TRUE;
}

#ifdef CR_SMMU_REAL_CORE
STATIC UINT32 TestSmmuHardwareRead32 (UINTN Address);
STATIC VOID TestSmmuHardwareWrite32 (UINTN Address, UINT32 Value);
#endif
UINT32 EFIAPI
MmioRead32 (
  IN UINTN Address
  )
{
#ifdef CR_SMMU_REAL_CORE
  return TestSmmuHardwareRead32 (Address);
#else
  (VOID)Address;
  return 0;
#endif
}

UINT32 EFIAPI
MmioWrite32 (
  IN UINTN  Address,
  IN UINT32 Value
  )
{
#ifdef CR_SMMU_REAL_CORE
  TestSmmuHardwareWrite32 (Address, Value);
#else
  (VOID)Address;
#endif
  return Value;
}

UINT64 EFIAPI
MmioWrite64 (
  IN UINTN  Address,
  IN UINT64 Value
  )
{
#ifdef CR_SMMU_REAL_CORE
  TestSmmuHardwareWrite32 (Address, (UINT32)Value);
  TestSmmuHardwareWrite32 (Address + 4, (UINT32)(Value >> 32));
#else
  (VOID)Address;
#endif
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
STATIC UINTN                mEvidenceCalls;
STATIC UINT16               mEvidenceFailSid;
STATIC EFI_STATUS           mEvidenceStatus;
STATIC UINTN                mBlockCalls;
STATIC SmmuDomain          *mBlockLog[CR_SMMU_MAX_DOMAINS];
STATIC UINTN                mFailBlockCall;
STATIC UINTN                mDetachCalls;
STATIC UINTN                mAttachCalls;
STATIC CR_STATUS            mAttachStatus;
STATIC BOOLEAN              mAttachPoisoned;
STATIC UINTN                mWriteBackCalls;
STATIC UINTN                mWriteBackInvalidateCalls;
STATIC UINTN                mInvalidateCalls;
STATIC VOID                *mLastCacheAddress;
STATIC UINTN                mLastCacheLength;
STATIC UINTN                mGetDescriptorCalls;
STATIC UINTN                mSetMemoryAttributeCalls;
STATIC UINTN                mFailMemoryAttributeCall;
STATIC UINT64               mLastMemoryAttributes;
STATIC EFI_STATUS           mSetMemoryAttributeStatus;
STATIC EFI_STATUS           mGetDescriptorStatus;
STATIC EFI_GCD_MEMORY_TYPE  mDescriptorType;
STATIC UINT64               mDescriptorLength;
STATIC UINT64               mDescriptorAttributes = EFI_MEMORY_WB | EFI_MEMORY_XP;
STATIC UINT64               mDescriptorCapabilities;
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

STATIC CrTargetSmmuContext mValidationTarget;
CONST CrTargetSmmuContext *CrDalGetSmmuContext (VOID)
{
  return &mValidationTarget;
}

#ifndef CR_SMMU_DISPLAY_TARGET_OVERRIDE
CONST CR_MDSS_TARGET *CrDalGetMdssContext(VOID) {return NULL;}
#endif
VOID *EFIAPI SetMem(VOID *Buffer,UINTN Length,UINT8 Value)
{ return memset(Buffer,Value,Length); }

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
  if (Protocol == &gEfiDmaIdentityCrProtocolGuid) {
    *Interface = Handle == (EFI_HANDLE)(UINTN)0xD0 ? mIdentity : NULL;
    return *Interface == NULL ? EFI_UNSUPPORTED : EFI_SUCCESS;
  }
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
  Descriptor->Length       = mDescriptorLength;
  Descriptor->GcdMemoryType = mDescriptorType;
  /* Common DMA buffers use WC: on AArch64 that is normal non-cacheable
     memory, whereas EFI_MEMORY_UC is Device memory. */
  Descriptor->Capabilities = mDescriptorCapabilities;
  Descriptor->Attributes   = mDescriptorAttributes;
  return mGetDescriptorStatus;
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
  if (mSetMemoryAttributeCalls == mFailMemoryAttributeCall) {
    return EFI_DEVICE_ERROR;
  }
  return mSetMemoryAttributeStatus;
}

STATIC UINTN mFaultCaptureCalls;
STATIC CR_STATUS mFaultCaptureStatus;
#ifndef CR_SMMU_REAL_CORE
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

CR_STATUS SmmuDumpFaultState (SmmuDomain *Domain) {
  assert(Domain==&mSmmu.Domains[0].Domain && mSmmu.Lock.Lock==EfiLockAcquired);
  assert(Domain->Tables); mFaultCaptureCalls++; return mFaultCaptureStatus;
}

CR_STATUS
SmmuQueryMappingEvidence (SmmuDomain *Domain, UINT64 Iova, UINT64 Physical,
                         UINTN Pages, UINT32 Access, SmmuMappingEvidence *Evidence)
{
  assert(mSmmu.AfterExitBootServices && mSmmu.Lock.Lock == EfiLockAcquired);
  assert(Domain && Domain->Device == &mSmmu.Device && Domain->Active);
  assert(Iova >= SMMU_IOVA_BASE && Pages && Access == SMMU_ACCESS_READ);
  mEvidenceCalls++;
  ZeroMem(Evidence, sizeof(*Evidence));
  if (Domain->Sid == mEvidenceFailSid) return mEvidenceStatus;
  Evidence->Base = mSmmu.Device.Base; Evidence->Sid = Domain->Sid;
  Evidence->Iova = Iova; Evidence->Physical = Physical;
  Evidence->Bytes = Pages * SMMU_PAGE_SIZE; Evidence->Access = Access;
  return CR_SUCCESS;
}

CR_STATUS SmmuQueryRevokedMapping(SmmuDomain *Domain, UINT64 Iova,
                                 UINTN Pages, SmmuMappingEvidence *Evidence) {
  assert(mSmmu.ExitComplete && mSmmu.Lock.Lock == EfiLockAcquired);
  BOOLEAN Revoked=FALSE;
  for(UINTN I=0;I<mBlockCalls;++I)
    if(mBlockLog[I]==Domain&&I+1!=mFailBlockCall)Revoked=TRUE;
  for(UINTN I=0;I<mSetCalls;++I)
    if(mSetLog[I].Domain==Domain&&mSetLog[I].Iova==Iova&&
       mSetLog[I].Pages==Pages&&!mSetLog[I].Access&&I+1!=mFailSetCall1&&I+1!=mFailSetCall2)
      Revoked=TRUE;
  ZeroMem(Evidence,sizeof(*Evidence));
  if(!Revoked||Domain->Sid==mEvidenceFailSid)return CR_DEVICE_ERROR;
  Evidence->Sid=Domain->Sid;Evidence->Iova=Iova;Evidence->Bytes=Pages*SMMU_PAGE_SIZE;
  return CR_SUCCESS;
}

CR_STATUS
SmmuBlock (
  SmmuDomain *Domain
  )
{
  assert (Domain != NULL);
  assert (mBlockCalls < ARRAY_SIZE (mBlockLog));
  mBlockLog[mBlockCalls] = Domain;
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
  Domain->Failed  = mAttachPoisoned;
  return mAttachStatus;
}

CR_STATUS
SmmuAttachCoherent (
  SmmuDevice *Device,
  SmmuDomain *Domain,
  UINT16 Sid,
  VOID *Tables,
  UINT64 TableAddress
  )
{
  CR_STATUS Status = SmmuAttach (Device, Domain, Sid, Tables, TableAddress);
  Domain->CacheCoherent = TRUE;
  return Status;
}

CR_STATUS SmmuAcquireStoppedStream(SmmuDevice *Device,SmmuDomain *Domain,
    UINT16 Sid,VOID *Tables,UINT64 Address,BOOLEAN Coherent,
    SmmuVerifyStopped Check,VOID *Context) {
  CR_STATUS Status=Check(Context);
  if(CR_ERROR(Status))return Status;
  return Coherent?SmmuAttachCoherent(Device,Domain,Sid,Tables,Address):
                  SmmuAttach(Device,Domain,Sid,Tables,Address);
}
#endif

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
  mEvidenceCalls               = 0;
  mEvidenceFailSid             = 0xFFFF;
  mEvidenceStatus              = EFI_DEVICE_ERROR;
  ZeroMem (mBlockLog, sizeof (mBlockLog));
  mFailBlockCall               = 0;
  mDetachCalls                 = 0;
  mAttachCalls                 = 0;
  mAttachStatus                = CR_SUCCESS;
  mAttachPoisoned              = FALSE;
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
  mSmmu.Device.Stage1OutputAddressBits = 64;
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
  mFailMemoryAttributeCall = 0;
  mLastMemoryAttributes    = 0;
  mSetMemoryAttributeStatus = EFI_SUCCESS;
  mGetDescriptorStatus      = EFI_SUCCESS;
  mDescriptorType           = EfiGcdMemoryTypeSystemMemory;
  mDescriptorLength         = sizeof (mPagePool);
  mDescriptorAttributes     = EFI_MEMORY_WB | EFI_MEMORY_XP;
  mDescriptorCapabilities   = EFI_MEMORY_UC | EFI_MEMORY_WC | EFI_MEMORY_WB | EFI_MEMORY_XP;
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
  mSmmu.Domains[Index].Domain.Sid = Sid;
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
  mSmmu.Device.Stage1OutputAddressBits = 32;
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
  mSmmu.Device.Stage1OutputAddressBits = 64;
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
            EDKII_IOMMU_ATTRIBUTE_MEMORY_WRITE_COMBINE
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
  assert ((mLastMemoryAttributes & EFI_MEMORY_WC) != 0);
  assert ((mLastMemoryAttributes & EFI_MEMORY_UC) == 0);
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

  /* If the allocator temporarily refuses the release, retain ownership in
     normal non-cacheable memory so a retry never restores Device memory. */
  ResetState ();
  AddDomain (0, 0, 0x0100, 0x1C01);
  assert (CrSmmuAllocateBuffer (
            &mIoMmuProtocol,
            AllocateAnyPages,
            EfiBootServicesData,
            1,
            &HostAddress,
            0
            ) == EFI_SUCCESS);
  mFreeStatus = EFI_DEVICE_ERROR;
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 1, HostAddress) == EFI_DEVICE_ERROR);
  assert (mLastMemoryAttributes == (EFI_MEMORY_WC | EFI_MEMORY_XP));
  mFreeStatus = EFI_SUCCESS;
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 1, HostAddress) == EFI_SUCCESS);
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

  /* A long-lived display map must not block later UFS/PCIe attachment.
     The newly attached domain receives no access to that existing IOVA. */
  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0x2800);
  {
    STATIC UINT8 Buffer[SMMU_PAGE_SIZE] __attribute__((aligned(SMMU_PAGE_SIZE)));
    CR_SMMU_MAPPING *Map = MapReadBuffer (Buffer, sizeof (Buffer));
    ResetCalls ();
    assert (AttachStreamLocked (0, 0x0000, 0x1C00) == EFI_SUCCESS);
    assert (mAttachCalls == 1 && mSmmu.Domains[1].InUse);
    assert (mSetCalls == 0 && Map->DomainAccess[1] == 0);
    assert (CrSmmuSetAttributeById (
              &mIoMmuProtocol, mSmmu.Device.Base, 0x2800, Map,
              EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
    assert (Map->DomainAccess[0] == EDKII_IOMMU_ACCESS_READ &&
            Map->DomainAccess[1] == 0);
    ResetCalls ();
    assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
    assert (mSetCalls == 2);
  }

  ResetState ();
  Status = AttachStreamLocked (0, 0x0100, 0x1C01);
  assert (Status == EFI_SUCCESS && mAttachCalls == 1);
  assert (mLastMemoryType == EfiReservedMemoryType);

  /* Rejection before MMIO owns no domain; rollback can report NOT_FOUND. */
  ResetState ();
  mAttachStatus = CR_BUSY;
  assert (AttachStreamLocked (1, 0x0100, 0x1C81) == EFI_ACCESS_DENIED);
  assert (mFreeCalls == 1);
  assert (CrSmmuBlockSegment (&mCrProtocol, 1) == EFI_NOT_FOUND);

  /* A failed hardware write must keep its segment and tables quarantined. */
  ResetState ();
  mAttachStatus = CR_DEVICE_ERROR;
  mAttachPoisoned = TRUE;
  assert (AttachStreamLocked (1, 0x0100, 0x1C81) == EFI_DEVICE_ERROR);
  assert (mSmmu.Domains[0].Poisoned && !mSmmu.Domains[0].InUse);
  assert (mSmmu.Domains[0].Segment == 1 && mSmmu.Domains[0].Sid == 0x1C81);
  assert (CrSmmuBlockSegment (&mCrProtocol, 0) == EFI_NOT_FOUND);
  assert (CrSmmuBlockSegment (&mCrProtocol, 1) == EFI_DEVICE_ERROR);
  assert (CrSmmuBlockSegment (&mCrProtocol, 1) == EFI_DEVICE_ERROR);
  assert (AttachStreamLocked (1, 0x0100, 0x1C81) == EFI_DEVICE_ERROR);
  assert (mAttachCalls == 1 && mFreeCalls == 0 && mDetachCalls == 0);

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

STATIC VOID
TestDisplayHandoff (
  VOID
  )
{
  CR_SMMU_MAPPING *Display;
  CR_SMMU_MAPPING *Transient;
  VOID           *Mapping;
  EFI_PHYSICAL_ADDRESS Device;
  UINTN Bytes;

  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0x2800);
  Display = MapReadBuffer (mPagePool, SMMU_PAGE_SIZE + 1);
  /* System/boot memory cannot survive just because a consumer asks. The
     permission must cover both pages, including the rounded trailing page. */
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_UNSUPPORTED);
  mDescriptorType = EfiGcdMemoryTypeReserved;
  mDescriptorLength = SMMU_PAGE_SIZE + 1;
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_UNSUPPORTED);
  mDescriptorLength = sizeof (mPagePool);
  mGetDescriptorStatus = EFI_DEVICE_ERROR;
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_DEVICE_ERROR);
  mGetDescriptorStatus = EFI_SUCCESS;
  assert (CrSmmuPreserveReadOnlyMapping (NULL, Display) == EFI_INVALID_PARAMETER);
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, NULL) == EFI_INVALID_PARAMETER);
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, mPagePool) == EFI_INVALID_PARAMETER);
  Display->Operation = EdkiiIoMmuOperationBusMasterCommonBuffer;
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_UNSUPPORTED);
  Display->Operation = EdkiiIoMmuOperationBusMasterRead64;
  Display->DeviceWriteGranted = TRUE;
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_UNSUPPORTED);
  Display->DeviceWriteGranted = FALSE;
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_SUCCESS);
  assert (Display->PreserveAtExit);
  /* A mapping alone does not retain a stream which has no READ grant. */
  ResetCalls ();
  CrSmmuExitBootServices (NULL, NULL);
  assert (mBlockCalls == 1 && mSetCalls == 0);
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_INVALID_PARAMETER);

  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0x2800);
  mDescriptorType = EfiGcdMemoryTypeReserved;
  Bytes = 64;
  assert (CrSmmuMap (&mIoMmuProtocol, EdkiiIoMmuOperationBusMasterWrite,
                     mPagePool + SMMU_PAGE_SIZE, &Bytes, &Device, &Mapping) == EFI_SUCCESS);
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Mapping) == EFI_UNSUPPORTED);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Mapping) == EFI_SUCCESS);

  for (UINTN Failure = 0; Failure < 4; Failure++) {
    ResetState ();
    AddDomain (0, 0, 0xFFFF, 0x2800);
    AddDomain (1, 0, 0xFFFF, 0x2802);
    AddDomain (2, 0, 0xFFFF, 0x00E0);
    Display = MapReadBuffer (mPagePool, SMMU_PAGE_SIZE);
    Transient = MapReadBuffer (mPagePool + SMMU_PAGE_SIZE, SMMU_PAGE_SIZE);
    assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base, 0x2800,
                                    Display, EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
    assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base, 0x2802,
                                    Display, EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
    assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base, 0x2800,
                                    Transient, EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
    mDescriptorType = EfiGcdMemoryTypeReserved;
    assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_SUCCESS);
    ResetCalls ();
    if (Failure == 1) mFailSetCall1 = 1;
    if (Failure == 2) mSmmu.Domains[0].Poisoned = TRUE;
    if (Failure == 3) mSmmu.Domains[0].Domain.Failed = TRUE;
    CrSmmuExitBootServices (NULL, NULL);
    /* Both display SIDs can fetch only the reserved framebuffer. Storage
       is blocked; a revoke failure/poison blocks only the affected display SID. */
    assert (mBlockCalls == (Failure == 0 ? 1 : 2));
    assert (mBlockLog[mBlockCalls - 1] == &mSmmu.Domains[2].Domain);
    if (Failure != 0) assert (mBlockLog[0] == &mSmmu.Domains[0].Domain);
    if (Failure <= 1) {
      assert (mSetCalls == 1 && mSetLog[0].Iova == Transient->IovaBase);
      assert (mSetLog[0].Access == 0 && mSetLog[0].Domain == &mSmmu.Domains[0].Domain);
    }
    if (Failure == 0) assert (Transient->DomainAccess[0] == 0);
    assert (Display->DomainAccess[1] == EDKII_IOMMU_ACCESS_READ);
    assert (mFreeCalls == 0 && mDetachCalls == 0 && mAllocateCalls == 0);
  }

  /* An attempted unload withdraws the handoff even if Unmap fails halfway.
     A subsequent exit must not preserve the remaining stale translation. */
  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0x2800);
  Display = MapReadBuffer (mPagePool, SMMU_PAGE_SIZE);
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base, 0x2800,
                                  Display, EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
  mDescriptorType = EfiGcdMemoryTypeReserved;
  assert (CrSmmuPreserveReadOnlyMapping (&mCrProtocol, Display) == EFI_SUCCESS);
  ResetCalls ();
  mFailSetCall1 = 1;
  assert (CrSmmuUnmap (&mIoMmuProtocol, Display) == EFI_DEVICE_ERROR);
  assert (!Display->PreserveAtExit && Display->InUse);
  ResetCalls ();
  CrSmmuExitBootServices (NULL, NULL);
  assert (mBlockCalls == 1 && mSetCalls == 0);
}

STATIC UINTN mObserverCalls;
STATIC EFI_STATUS mObserverStatus, mObserverQueryStatus;
STATIC EFI_SMMU_CR_MAPPING_EVIDENCE mObserved;
STATIC VOID EFIAPI ExitObserver(VOID *Context, EFI_STATUS Status)
{
  assert(mSmmu.AfterExitBootServices && mSmmu.ExitComplete);
  assert(mSmmu.Lock.Lock == EfiLockReleased);
  mObserverCalls++; mObserverStatus = Status;
  mObserverQueryStatus = CrSmmuQueryExitMapping(&mCrProtocol, Context, &mObserved);
  assert(CrSmmuSetExitObserver(&mCrProtocol, Context, NULL, NULL) == EFI_ACCESS_DENIED);
}

STATIC UINTN mPrepareCalls;
STATIC EFI_STATUS mPrepareStatus;
STATIC EFI_STATUS EFIAPI PrepareObserver(VOID *Context) {
  assert(Context==mPagePool&&mSmmu.AfterExitBootServices&&!mSmmu.ExitComplete);
  assert(mSmmu.Lock.Lock==EfiLockReleased&&!mSetCalls&&!mBlockCalls);
  assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,Context,NULL,NULL)==EFI_ACCESS_DENIED);
  ++mPrepareCalls;return mPrepareStatus;
}

STATIC VOID TestExitEvidence(VOID)
{
  EFI_SMMU_CR_MAPPING_EVIDENCE E, Empty = {0};
  CR_SMMU_MAPPING *Display, *Transient;
  for (UINTN Failure=0;Failure<5;++Failure) {
    ResetState(); mObserverCalls=0;
    AddDomain(0,0,0xFFFF,0x2800); AddDomain(1,0,0xFFFF,0x2802);
    Display=MapReadBuffer(mPagePool+8,SMMU_PAGE_SIZE+1);
    Transient=MapReadBuffer(mPagePool+4*SMMU_PAGE_SIZE,SMMU_PAGE_SIZE);
    for (UINTN I=0;I<2;++I)
      assert(CrSmmuSetAttributeById(&mIoMmuProtocol,mSmmu.Device.Base,
             mSmmu.Domains[I].Sid,Display,EDKII_IOMMU_ACCESS_READ)==EFI_SUCCESS);
    assert(CrSmmuSetAttributeById(&mIoMmuProtocol,mSmmu.Device.Base,0x2800,
             Transient,EDKII_IOMMU_ACCESS_READ)==EFI_SUCCESS);
    mDescriptorType=EfiGcdMemoryTypeReserved;
    assert(CrSmmuPreserveReadOnlyMapping(&mCrProtocol,Display)==EFI_SUCCESS);
    assert(CrSmmuSetExitObserver(&mCrProtocol,Display,ExitObserver,Display)==EFI_SUCCESS);
    assert(CrSmmuSetExitObserver(&mCrProtocol,Display,ExitObserver,Display)==EFI_SUCCESS);
    assert(CrSmmuSetExitObserver(&mCrProtocol,Display,ExitObserver,Transient)==EFI_ALREADY_STARTED);
    assert(CrSmmuQueryExitMapping(&mCrProtocol,Display,&E)==EFI_NOT_READY);
    assert(!memcmp(&E,&Empty,sizeof(E)) && !mEvidenceCalls);
    ResetCalls();
    if(Failure==1)mFailSetCall1=1;
    if(Failure==2)mEvidenceFailSid=0x2802;
    if(Failure==3)mSmmu.Domains[1].Poisoned=TRUE;
    if(Failure==4){AddDomain(2,0,0xFFFF,0xE0);mFailBlockCall=1;}
    CrSmmuExitBootServices(NULL,NULL);
    assert(mObserverCalls==1 && mSmmu.ExitComplete);
    if(Failure==0||Failure==4) {
      assert(mObserverQueryStatus==EFI_SUCCESS && mObserved.StreamCount==2);
      assert(mObserved.Size==sizeof(mObserved) && mObserved.Revision==1);
      assert(mObserved.MappingId==(UINT32)(Display-mSmmu.Mappings)+1);
      assert(mObserved.HostAddress==(UINT64)(UINTN)(mPagePool+8));
      assert(mObserved.DeviceAddress==Display->IovaBase+8);
      assert(mObserved.NumberOfBytes==SMMU_PAGE_SIZE+1 && mObserved.PageBytes==2*SMMU_PAGE_SIZE);
      assert(mObserved.Streams[0].Sid==0x2800 && mObserved.Streams[1].Sid==0x2802);
      assert(mObserverStatus==(Failure==4?EFI_DEVICE_ERROR:EFI_SUCCESS));
    } else {
      assert(EFI_ERROR(mObserverQueryStatus));
      assert(!memcmp(&mObserved,&Empty,sizeof(Empty)));
    }
    assert(!mAllocateCalls && !mFreeCalls && !mDetachCalls);
    assert(CrSmmuQueryExitMapping(&mCrProtocol,Transient,&E)==EFI_ACCESS_DENIED);
    assert(CrSmmuQueryExitMapping(&mCrProtocol,mPagePool,&E)==EFI_NOT_FOUND);
    assert(CrSmmuQueryExitMapping(NULL,Display,&E)==EFI_INVALID_PARAMETER);
    assert(CrSmmuQueryExitMapping(&mCrProtocol,Display,NULL)==EFI_INVALID_PARAMETER);
    CrSmmuExitBootServices(NULL,NULL);assert(mObserverCalls==1);
  }
  // The exact original stream set survives the EBS grant-ledger clears.
  for(UINTN Preserve=0;Preserve<2;++Preserve) {
    ResetState();AddDomain(0,0,0xFFFF,0x2800);AddDomain(1,0,0xFFFF,0x2802);
    Display=MapReadBuffer(mPagePool,SMMU_PAGE_SIZE);
    Transient=MapReadBuffer(mPagePool+4*SMMU_PAGE_SIZE,SMMU_PAGE_SIZE);
    for(UINTN I=0;I<2;++I) {
      assert(CrSmmuSetAttributeById(&mIoMmuProtocol,mSmmu.Device.Base,mSmmu.Domains[I].Sid,Transient,EDKII_IOMMU_ACCESS_READ)==EFI_SUCCESS);
      if(Preserve)assert(CrSmmuSetAttributeById(&mIoMmuProtocol,mSmmu.Device.Base,mSmmu.Domains[I].Sid,Display,EDKII_IOMMU_ACCESS_READ)==EFI_SUCCESS);
    }
    if(Preserve){mDescriptorType=EfiGcdMemoryTypeReserved;assert(CrSmmuPreserveReadOnlyMapping(&mCrProtocol,Display)==EFI_SUCCESS);}
    assert(CrSmmuQueryExitRevokedMapping(&mCrProtocol,Transient,&E)==EFI_NOT_READY);
    ResetCalls();CrSmmuExitBootServices(NULL,NULL);
    assert(CrSmmuQueryExitRevokedMapping(&mCrProtocol,Transient,&E)==EFI_SUCCESS);
    assert(E.StreamCount==2&&E.MappingId==(UINT32)(Transient-mSmmu.Mappings)+1);
    assert(E.Streams[0].Sid==0x2800&&E.Streams[1].Sid==0x2802&&!E.Streams[0].Access&&!E.Streams[0].Physical);
    if(Preserve)assert(!Transient->DomainAccess[0]&&!Transient->DomainAccess[1]);
    assert(Transient->ExitDomainAccess[0]==EDKII_IOMMU_ACCESS_READ&&Transient->ExitDomainAccess[1]==EDKII_IOMMU_ACCESS_READ);
    mEvidenceFailSid=0x2802;
    assert(CrSmmuQueryExitRevokedMapping(&mCrProtocol,Transient,&E)==EFI_DEVICE_ERROR);
    assert(!memcmp(&E,&Empty,sizeof(E)));
    assert(!mAllocateCalls&&!mFreeCalls&&!mDetachCalls);
  }
  // Whole-aperture USB revocation must not depend on a read-only Map token.
  for(UINTN Failure=0;Failure<5;++Failure) {
    SmmuMappingEvidence Full,Zero={0};
    ResetState();AddDomain(0,0,0xFFFF,0);ResetCalls();
    assert(CrSmmuQueryExitRevokedDomain(&mCrProtocol,0,0xFFFF,0,&Full)==EFI_NOT_READY);
    if(Failure==1)mFailBlockCall=1;
    CrSmmuExitBootServices(NULL,NULL);
    if(Failure==2)mSmmu.Domains[0].Poisoned=TRUE;
    if(Failure==3)mSmmu.Domains[0].PreservedAtExit=TRUE;
    if(Failure==4)mEvidenceFailSid=0;
    EFI_STATUS Status=CrSmmuQueryExitRevokedDomain(&mCrProtocol,0,0xFFFF,0,&Full);
    if(!Failure) {
      assert(Status==EFI_SUCCESS&&Full.Sid==0&&Full.Iova==SMMU_IOVA_BASE&&Full.Bytes==SMMU_IOVA_SIZE&&!Full.Physical&&!Full.Access);
    } else assert(EFI_ERROR(Status)&&!memcmp(&Full,&Zero,sizeof(Full)));
    assert(CrSmmuQueryExitRevokedDomain(&mCrProtocol,1,0xFFFF,0,&Full)==EFI_ACCESS_DENIED);
    assert(CrSmmuQueryExitRevokedDomain(&mCrProtocol,0,0,0,&Full)==EFI_ACCESS_DENIED);
    assert(CrSmmuQueryExitRevokedDomain(&mCrProtocol,0,0xFFFF,1,&Full)==EFI_NOT_FOUND);
    assert(!memcmp(&Full,&Zero,sizeof(Full)));
    assert(CrSmmuQueryExitRevokedDomain(NULL,0,0xFFFF,0,&Full)==EFI_INVALID_PARAMETER);
    assert(CrSmmuQueryExitRevokedDomain(&mCrProtocol,0,0xFFFF,0,NULL)==EFI_INVALID_PARAMETER);
    assert(!mAllocateCalls&&!mFreeCalls&&!mDetachCalls);
  }
  for(UINTN Failure=0;Failure<3;++Failure) {
    ResetState();AddDomain(0,0,0xFFFF,0x2800);mPrepareCalls=0;
    mPrepareStatus=Failure==2?EFI_WARN_STALE_DATA:Failure?EFI_DEVICE_ERROR:EFI_SUCCESS;
    for(UINTN I=1;I<=8;++I)
      assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,(VOID*)I,PrepareObserver,mPagePool)==EFI_SUCCESS);
    assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,(VOID*)9,PrepareObserver,mPagePool)==EFI_OUT_OF_RESOURCES);
    assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,(VOID*)1,PrepareObserver,mPagePool)==EFI_SUCCESS);
    assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,(VOID*)1,PrepareObserver,NULL)==EFI_ALREADY_STARTED);
    for(UINTN I=2;I<=8;++I)assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,(VOID*)I,NULL,NULL)==EFI_SUCCESS);
    assert(CrSmmuSetExitPrepareObserver(&mCrProtocol,(VOID*)2,NULL,NULL)==EFI_NOT_FOUND);
    ResetCalls();CrSmmuExitBootServices(NULL,NULL);
    assert(mPrepareCalls==1&&mBlockCalls==1&&mSmmu.ExitComplete);
    assert(mSmmu.ExitStatus==(Failure==2?EFI_NOT_READY:mPrepareStatus)&&!mAllocateCalls&&!mFreeCalls);
    CrSmmuExitBootServices(NULL,NULL);assert(mPrepareCalls==1);
  }
  ResetState();
  for(UINTN I=1;I<=8;++I)
    assert(CrSmmuSetExitObserver(&mCrProtocol,(VOID*)I,ExitObserver,NULL)==EFI_SUCCESS);
  assert(CrSmmuSetExitObserver(&mCrProtocol,(VOID*)9,ExitObserver,NULL)==EFI_OUT_OF_RESOURCES);
  assert(CrSmmuSetExitObserver(&mCrProtocol,(VOID*)4,NULL,NULL)==EFI_SUCCESS);
  assert(CrSmmuSetExitObserver(&mCrProtocol,(VOID*)4,NULL,NULL)==EFI_NOT_FOUND);
  assert(CrSmmuSetExitObserver(&mCrProtocol,(VOID*)9,ExitObserver,NULL)==EFI_SUCCESS);
  assert(CrSmmuSetExitObserver(NULL,(VOID*)9,NULL,NULL)==EFI_INVALID_PARAMETER);
  assert(CrSmmuSetExitObserver(&mCrProtocol,NULL,NULL,NULL)==EFI_INVALID_PARAMETER);
  for(UINTN I=1;I<=9;++I)if(I!=4)
    assert(CrSmmuSetExitObserver(&mCrProtocol,(VOID*)I,NULL,NULL)==EFI_SUCCESS);
  mObserverCalls=0;CrSmmuExitBootServices(NULL,NULL);assert(!mObserverCalls);
}

STATIC VOID
TestScopedDetachAndMappedFree (VOID)
{
  STATIC UINT8 Buffer[SMMU_PAGE_SIZE] __attribute__((aligned(SMMU_PAGE_SIZE)));
  CR_SMMU_MAPPING *Map;
  VOID *Host, *Token;
  UINTN Size;
  EFI_PHYSICAL_ADDRESS Device;
  UINTN FreeBefore, AttrBefore;

  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0x00E0);
  AddDomain (1, 0, 0x0100, 0x1C01);
  AddDomain (2, 1, 0x0100, 0x1C81);
  Map = MapReadBuffer (Buffer, sizeof (Buffer));
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
           0x00E0, Map, EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
           0x1C81, Map, EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
  assert (CrSmmuBlockSegment (&mCrProtocol, 1) == EFI_ACCESS_DENIED);
  assert (mBlockCalls == 0);
  assert (CrSmmuBlockSegment (&mCrProtocol, 0) == EFI_SUCCESS);
  assert (mBlockCalls == 1 && mBlockLog[0] == &mSmmu.Domains[1].Domain);
  assert (mSmmu.Domains[0].InUse && mSmmu.Domains[2].InUse);
  assert (CrSmmuBlockSegment (&mCrProtocol, 0) == EFI_NOT_FOUND);
  assert (AttachStreamLocked (0, 0x0100, 0x00E0) == EFI_ACCESS_DENIED);
  assert (AttachStreamLocked (1, 0xFFFF, 0x1C81) == EFI_ACCESS_DENIED);
  assert (AttachStreamLocked (1, 0x0200, 0x1C81) == EFI_ACCESS_DENIED);
  assert (AttachStreamLocked (1, 0x0100, 0x1C81) == EFI_SUCCESS);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Map) == EFI_SUCCESS);
  assert (CrSmmuBlockSegment (&mCrProtocol, 1) == EFI_SUCCESS);

  for (UINTN Bounce = 0; Bounce < 2; Bounce++) {
    ResetState ();
    AddDomain (0, 0, 0x0100, 0x1C01);
    assert (CrSmmuAllocateBuffer (&mIoMmuProtocol, AllocateAnyPages,
             EfiBootServicesData, 2, &Host, 0) == EFI_SUCCESS);
    Size = SMMU_PAGE_SIZE;
    assert (CrSmmuMap (&mIoMmuProtocol,
             Bounce ? EdkiiIoMmuOperationBusMasterWrite : EdkiiIoMmuOperationBusMasterCommonBuffer,
             (UINT8 *)Host + SMMU_PAGE_SIZE, &Size, &Device, &Token) == EFI_SUCCESS);
    FreeBefore = mFreeCalls; AttrBefore = mSetMemoryAttributeCalls;
    assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 2, Host) == EFI_ACCESS_DENIED);
    assert (mFreeCalls == FreeBefore && mSetMemoryAttributeCalls == AttrBefore);
    assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
             0x1C01, Token, EDKII_IOMMU_ACCESS_WRITE) == EFI_SUCCESS);
    mFailSetCall1 = mSetCalls + 1;
    assert (CrSmmuUnmap (&mIoMmuProtocol, Token) == EFI_DEVICE_ERROR);
    assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 2, Host) == EFI_ACCESS_DENIED);
    assert (mFreeCalls == FreeBefore && mSetMemoryAttributeCalls == AttrBefore);
    mFailSetCall1 = 0;
    assert (CrSmmuUnmap (&mIoMmuProtocol, Token) == EFI_SUCCESS);
    assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 2, Host) == EFI_SUCCESS);
  }

  ResetState ();
  mAttachStatus = CR_DEVICE_ERROR; mAttachPoisoned = TRUE;
  assert (AttachStreamLocked (0, 0x0100, 0x1C01) == EFI_DEVICE_ERROR);
  assert (mSmmu.Domains[0].Poisoned && !mSmmu.Domains[0].InUse);
  CrSmmuExitBootServices (NULL, NULL);
  assert (mBlockCalls == 1 && mDetachCalls == 0 && mFreeCalls == 0);
}

STATIC VOID
TestTargetActlrContract (VOID)
{
  STATIC CONST SmmuActlrSetting Setting = { 0xE0, 0, 0 };
  CONST CrTargetSmmuContext *Target = NULL;
  ZeroMem (&mValidationTarget, sizeof (mValidationTarget));
  mValidationTarget.Base = 0x15000000;
  mValidationTarget.Size = 0x100000;
  mValidationTarget.SegmentCount = 2;
  mValidationTarget.ActlrSettingCount = 1;
  assert (GetTargetSmmuContext (&Target) == EFI_INVALID_PARAMETER);
  mValidationTarget.ActlrSettings = &Setting;
  assert (GetTargetSmmuContext (&Target) == EFI_SUCCESS && Target == &mValidationTarget);
  mValidationTarget.ActlrSettingCount = 0;
  mValidationTarget.ActlrSettings = NULL;
  assert (GetTargetSmmuContext (&Target) == EFI_SUCCESS);
  STATIC CONST UINT16 Allowed[] = { 0xE0, 0x1C00, 0x1C01 };
  STATIC CONST UINT16 Coherent[] = { 0xE0, 0x1C01 };
  STATIC CONST UINT16 Duplicate[] = { 0xE0, 0xE0 };
  STATIC CONST UINT16 Unknown[] = { 0x20 };
  mValidationTarget.DefaultStreamIds = Allowed;
  mValidationTarget.DefaultStreamCount = ARRAY_SIZE (Allowed);
  mValidationTarget.CoherentStreamCount = ARRAY_SIZE (Coherent);
  assert (GetTargetSmmuContext (&Target) == EFI_INVALID_PARAMETER);
  mValidationTarget.CoherentStreamIds = Coherent;
  assert (GetTargetSmmuContext (&Target) == EFI_SUCCESS);
  mValidationTarget.CoherentStreamIds = Duplicate;
  assert (GetTargetSmmuContext (&Target) == EFI_INVALID_PARAMETER);
  mValidationTarget.CoherentStreamIds = Unknown;
  mValidationTarget.CoherentStreamCount = ARRAY_SIZE (Unknown);
  assert (GetTargetSmmuContext (&Target) == EFI_INVALID_PARAMETER);
  mValidationTarget.CoherentStreamIds = Coherent;
  mValidationTarget.CoherentStreamCount = ARRAY_SIZE (Coherent);
  mValidationTarget.DefaultStreamCount = 0;
  assert (GetTargetSmmuContext (&Target) == EFI_INVALID_PARAMETER);
}

STATIC VOID
TestTargetCoherentAttach (VOID)
{
  STATIC CONST UINT16 Allowed[] = { 0xE0, 0x1C01, 0 };
  STATIC CONST UINT16 Coherent[] = { 0xE0, 0x1C01 };
  CrTargetSmmuContext Target = mTarget;
  ResetState ();
  Target.DefaultStreamIds = Allowed;
  Target.DefaultStreamCount = ARRAY_SIZE (Allowed);
  Target.CoherentStreamIds = Coherent;
  Target.CoherentStreamCount = ARRAY_SIZE (Coherent);
  mSmmu.Target = &Target;
  assert (AttachStreamLocked (0, 0xFFFF, 0xE0) == EFI_SUCCESS);
  assert (FindDomainBySid (0xE0)->Domain.CacheCoherent);
  assert (AttachStreamLocked (0, 0x0100, 0x1C01) == EFI_SUCCESS);
  assert (FindDomainBySid (0x1C01)->Domain.CacheCoherent);
  assert (AttachStreamLocked (0, 0xFFFF, 0) == EFI_SUCCESS);
  assert (!FindDomainBySid (0)->Domain.CacheCoherent);
  for (UINTN Index = 0; Index < 3; Index++) {
    FreeDomainTables (&mSmmu.Domains[Index]);
  }
}

STATIC VOID
TestSm8450UfsDmaPolicy (VOID)
{
  VOID *Host;
  VOID *Mapping;
  UINTN Bytes = SMMU_PAGE_SIZE;
  EFI_PHYSICAL_ADDRESS Device;

  ResetState ();
  mSmmu.Target = CrTargetGetActualSm8450SmmuContext ();
  assert (AttachStreamLocked (0, 0xFFFF, 0xE0) == EFI_SUCCESS);
  // The actual target pairs coherent E0 translation with CPU WB storage.
  assert (FindDomainBySid (0xE0)->Domain.CacheCoherent);
  assert (CrSmmuAllocateBuffer (&mIoMmuProtocol, AllocateAnyPages,
            EfiBootServicesData, 1, &Host,
            EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_SUCCESS);
  assert (mLastMemoryAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP));
  assert (CrSmmuMap (&mIoMmuProtocol, EdkiiIoMmuOperationBusMasterCommonBuffer64,
            Host, &Bytes, &Device, &Mapping) == EFI_SUCCESS);
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
            0xE0, Mapping, EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE)
          == EFI_SUCCESS);
  assert (mSetLog[mSetCalls - 1].Domain == &FindDomainBySid (0xE0)->Domain);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Mapping) == EFI_SUCCESS);
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 1, Host) == EFI_SUCCESS);
  assert (AttachStreamLocked (0, 0x0100, 0x1C01) == EFI_SUCCESS);
  assert (FindDomainBySid (0x1C01)->Domain.CacheCoherent);
  for (UINTN Index = 0; Index < 2; Index++) {
    FreeDomainTables (&mSmmu.Domains[Index]);
  }
}

STATIC VOID
TestCachedCommonBuffer (VOID)
{
  VOID *Host;
  VOID *Token;
  UINTN Bytes;
  UINTN Calls;
  EFI_PHYSICAL_ADDRESS Device;
  CR_SMMU_MAPPING *Map;

  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0xE0);
  AddDomain (1, 0, 0xFFFF, 0);
  AddDomain (2, 0, 0x0100, 0x1C00);
  AddDomain (3, 0, 0x0100, 0x1C01);
  mSmmu.Domains[0].Domain.CacheCoherent = TRUE;
  mSmmu.Domains[2].Domain.CacheCoherent = TRUE;
  // A different original CPU attribute makes failed-free restoration visible.
  mDescriptorAttributes = EFI_MEMORY_WC | EFI_MEMORY_XP;
  assert (CrSmmuAllocateBuffer (&mIoMmuProtocol, AllocateAnyPages,
            EfiBootServicesData, 3, &Host,
            EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_SUCCESS);
  assert (mLastMemoryAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP));
  // Cache policy belongs to the allocation, including unaligned subranges.
  Bytes = 512;
  assert (CrSmmuMap (&mIoMmuProtocol, EdkiiIoMmuOperationBusMasterCommonBuffer64,
            (UINT8 *)Host + SMMU_PAGE_SIZE + 31, &Bytes, &Device, &Token) == EFI_SUCCESS);
  Map = Token;
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
            0xE0, Token, 3) == EFI_SUCCESS);
  Calls = mSetCalls;
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
            0, Token, 3) == EFI_UNSUPPORTED);
  assert (mSetCalls == Calls && Map->DomainAccess[1] == 0);
  // Reject mixed PCI streams before granting even the coherent first stream.
  assert (CrSmmuSetAttribute (&mIoMmuProtocol, mHandleSegment0, Token, 3)
          == EFI_UNSUPPORTED);
  assert (mSetCalls == Calls && Map->DomainAccess[2] == 0 && Map->DomainAccess[3] == 0);
  mSmmu.Domains[3].Domain.CacheCoherent = TRUE;
  assert (CrSmmuSetAttribute (&mIoMmuProtocol, mHandleSegment0, Token, 3) == EFI_SUCCESS);
  assert (Map->DomainAccess[2] == 3 && Map->DomainAccess[3] == 3);
  // Revocation must remain possible even if a domain is no longer coherent.
  mSmmu.Domains[3].Domain.CacheCoherent = FALSE;
  assert (CrSmmuSetAttribute (&mIoMmuProtocol, mHandleSegment0, Token, 0) == EFI_SUCCESS);
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
            0, Token, 0) == EFI_SUCCESS);
  Calls = mSetMemoryAttributeCalls;
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 3, Host) == EFI_ACCESS_DENIED);
  assert (mSetMemoryAttributeCalls == Calls);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Token) == EFI_SUCCESS);
  mFreeStatus = EFI_DEVICE_ERROR;
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 3, Host) == EFI_DEVICE_ERROR);
  assert (mLastMemoryAttributes == (EFI_MEMORY_WB | EFI_MEMORY_XP));
  // A failed free retains a usable WB allocation with the same grant policy.
  Bytes = SMMU_PAGE_SIZE;
  assert (CrSmmuMap (&mIoMmuProtocol, EdkiiIoMmuOperationBusMasterCommonBuffer,
            Host, &Bytes, &Device, &Token) == EFI_SUCCESS);
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
            0, Token, 3) == EFI_UNSUPPORTED);
  assert (CrSmmuSetAttributeById (&mIoMmuProtocol, mSmmu.Device.Base,
            0xE0, Token, 3) == EFI_SUCCESS);
  assert (CrSmmuUnmap (&mIoMmuProtocol, Token) == EFI_SUCCESS);
  mFreeStatus = EFI_SUCCESS;
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 3, Host) == EFI_SUCCESS);
  assert (mLastMemoryAttributes == (EFI_MEMORY_WC | EFI_MEMORY_XP));

  ResetState ();
  mDescriptorCapabilities &= ~EFI_MEMORY_WB;
  assert (CrSmmuAllocateBuffer (&mIoMmuProtocol, AllocateAnyPages,
            EfiBootServicesData, 1, &Host,
            EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_UNSUPPORTED);
  assert (Host == NULL && mFreeCalls == 1 && mSetMemoryAttributeCalls == 0);
  ResetState ();
  mSetMemoryAttributeStatus = EFI_DEVICE_ERROR;
  assert (CrSmmuAllocateBuffer (&mIoMmuProtocol, AllocateAnyPages,
            EfiBootServicesData, 1, &Host,
            EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_DEVICE_ERROR);
  assert (Host == NULL && mFreeCalls == 1 && !mSmmu.DmaBuffers[0].InUse);

  ResetState ();
  AddDomain (0, 0, 0xFFFF, 0xE0);
  assert (CrSmmuAllocateBuffer (&mIoMmuProtocol, AllocateAnyPages,
            EfiRuntimeServicesData, 1, &Host,
            EDKII_IOMMU_ATTRIBUTE_MEMORY_CACHED) == EFI_SUCCESS);
  mFreeStatus = EFI_DEVICE_ERROR;
  mFailMemoryAttributeCall = mSetMemoryAttributeCalls + 2;
  assert (CrSmmuFreeBuffer (&mIoMmuProtocol, 1, Host) == EFI_DEVICE_ERROR);
  // Failed attribute recovery quarantines storage instead of allowing DMA.
  assert (mSmmu.DmaBuffers[0].InUse && mSmmu.DmaBuffers[0].Busy);
  Bytes = SMMU_PAGE_SIZE;
  assert (CrSmmuMap (&mIoMmuProtocol, EdkiiIoMmuOperationBusMasterCommonBuffer64,
            Host, &Bytes, &Device, &Token) == EFI_UNSUPPORTED);
  assert (Token == NULL && Device == 0);
}

int
main (
  void
  )
{
  TestTargetActlrContract ();
  TestTargetCoherentAttach ();
  TestSm8450UfsDmaPolicy ();
  TestCachedCommonBuffer ();
  {
    ResetState();
    STATIC CONST CrTargetSmmuDmaDevice Device = {0x0A600000, 0x100000, 0};
    CrTargetSmmuContext Target = mTarget;
    Target.DmaDevices = &Device; Target.DmaDeviceCount = 1;
    mSmmu.Target = &Target;
    EFI_DMA_IDENTITY_CR_PROTOCOL Identity = {EFI_DMA_IDENTITY_CR_PROTOCOL_REVISION,
                                            0x15000000, 0x0A600000, 0x100000, 0};
    mIdentity = &Identity;
    UINTN Bytes = 4096;
    EFI_PHYSICAL_ADDRESS Iova;
    VOID *Token;
    AddDomain(0, 0, 0x0100, 0x1C01);
    assert(CrSmmuMap(&mIoMmuProtocol, EdkiiIoMmuOperationBusMasterRead64,
                     mPagePool, &Bytes, &Iova, &Token) == EFI_SUCCESS);
    assert(CrSmmuSetAttribute(&mIoMmuProtocol, (EFI_HANDLE)(UINTN)0xD0, Token,
                             EDKII_IOMMU_ACCESS_READ) == EFI_NOT_READY);
    AddDomain(1, 0, 0xFFFF, 0);
    assert(CrSmmuSetAttribute(&mIoMmuProtocol, (EFI_HANDLE)(UINTN)0xD0, Token,
                             EDKII_IOMMU_ACCESS_READ) == EFI_SUCCESS);
    assert(mSetLog[mSetCalls - 1].Domain == &mSmmu.Domains[1].Domain);
    Identity.Sid = 0x1C00;
    assert(CrSmmuSetAttribute(&mIoMmuProtocol, (EFI_HANDLE)(UINTN)0xD0, Token, 0) == EFI_ACCESS_DENIED);
    Identity.Sid = 0; Identity.ControllerSize = 0x200000;
    assert(CrSmmuSetAttribute(&mIoMmuProtocol, (EFI_HANDLE)(UINTN)0xD0, Token, 0) == EFI_ACCESS_DENIED);
    Identity.ControllerSize = 0x100000;
    mSmmu.Domains[1].Bdf = 0x100;
    assert(CrSmmuSetAttribute(&mIoMmuProtocol, (EFI_HANDLE)(UINTN)0xD0, Token, 0) == EFI_NOT_READY);
    mSmmu.Domains[1].Bdf = 0xFFFF;
    assert(CrSmmuUnmap(&mIoMmuProtocol, Token) == EFI_SUCCESS);
    mIdentity = NULL;
  }
  /* Pure native-identity fault capture rejects foreign/late queries without
     mutation, and remains available for poisoned domains with live DMA. */
  ResetState(); AddDomain(0,0,0xFFFF,0xE0);
  UINTN FaultBytes=16; VOID *FaultMap; EFI_PHYSICAL_ADDRESS FaultIova;
  assert(CrSmmuMap(&mIoMmuProtocol,EdkiiIoMmuOperationBusMasterRead64,mPagePool,
                   &FaultBytes,&FaultIova,&FaultMap)==EFI_SUCCESS);
  assert(CrSmmuSetAttributeById(&mIoMmuProtocol,mSmmu.Device.Base,0xE0,FaultMap,1)==EFI_SUCCESS);
  mSmmu.Domains[0].Poisoned=TRUE; mSmmu.Domains[0].Domain.Failed=TRUE;
  mFaultCaptureCalls=0; mFaultCaptureStatus=CR_SUCCESS;
  static UINT64 FaultTables[SMMU_TABLE_PAGES*4096/8]; mSmmu.Domains[0].Domain.Tables=FaultTables;
  CR_SMMU_STATE SavedState=mSmmu;
  UINTN SavedSet=mSetCalls, SavedBlock=mBlockCalls, SavedAlloc=mAllocateCalls, SavedFree=mFreeCalls;
  assert(CrSmmuDumpFault(&mCrProtocol,0,0xFFFF,0xE0)==EFI_SUCCESS && mFaultCaptureCalls==1);
  assert(!memcmp(&SavedState,&mSmmu,sizeof(mSmmu)));
  assert(mSetCalls==SavedSet && mBlockCalls==SavedBlock && mAllocateCalls==SavedAlloc && mFreeCalls==SavedFree);
  assert(CrSmmuDumpFault(NULL,0,0xFFFF,0xE0)==EFI_INVALID_PARAMETER);
  assert(CrSmmuDumpFault(&mCrProtocol,1,0xFFFF,0xE0)==EFI_NOT_FOUND);
  assert(CrSmmuDumpFault(&mCrProtocol,0,0x100,0xE0)==EFI_NOT_FOUND);
  assert(CrSmmuDumpFault(&mCrProtocol,0,0xFFFF,0xE1)==EFI_NOT_FOUND && mFaultCaptureCalls==1);
  mFaultCaptureStatus=CR_NOT_READY;
  assert(CrSmmuDumpFault(&mCrProtocol,0,0xFFFF,0xE0)==EFI_NOT_READY && mFaultCaptureCalls==2);
  mSmmu.AfterExitBootServices=TRUE;
  assert(CrSmmuDumpFault(&mCrProtocol,0,0xFFFF,0xE0)==EFI_ACCESS_DENIED && mFaultCaptureCalls==2);
  mSmmu.ExistingIoMmu=TRUE;
  assert(CrSmmuDumpFault(&mCrProtocol,0,0xFFFF,0xE0)==EFI_NOT_READY && mFaultCaptureCalls==2);
  TestPerDeviceIsolation ();
  TestPermissionRollback ();
  TestFailedMapQuarantinesBounce ();
  TestMapBoundsAndUnmapRetry ();
  TestWriteBounceAndFailedAuthorization ();
  TestCommonBufferAllocation ();
  TestAttachBlockAndExit ();
  TestDisplayHandoff ();
  TestScopedDetachAndMappedFree ();
  TestExitEvidence ();
  puts ("Crane SMMU adapter: isolation, bounce, allocation and EBS checks passed");
  #ifndef MDEPKG_NDEBUG
  assert (mDiagnosticCount > 0);
  assert (mSawMapDiagnostic && mSawEbsDiagnostic && mSawAttachDiagnostic);
  #endif
  return 0;
}
