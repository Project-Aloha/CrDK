/* Cross-layer regression: actual RAM publication -> actual DMA allocator.
 * Reuse the established SMMU firmware model without replacing production
 * AllocateBuffer/FreeBuffer or imposing a fake DMA success callback. MIT */
#define main CrSmmuExistingTestsMain
#define gBS RamSmmuBootServices
#define gDS RamSmmuDxeServices
#define CopyMem RamSmmuCopyMem
#define SetMem RamSmmuSetMem
#define DebugPrint RamSmmuDebugPrint
#define DebugPrintEnabled RamSmmuDebugPrintEnabled
#define DebugPrintLevelEnabled RamSmmuDebugPrintLevelEnabled
#define DebugAssert RamSmmuDebugAssert
#define DebugAssertEnabled RamSmmuDebugAssertEnabled
#include "SmmuCrDxeTest.c"
#undef main

STATIC EFI_GCD_MEMORY_SPACE_DESCRIPTOR mPublishedDescriptor;
STATIC EFI_STATUS EFIAPI RamDescriptor(
  EFI_PHYSICAL_ADDRESS Address,EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Descriptor)
{
  assert(Address==mPublishedDescriptor.BaseAddress);++mGetDescriptorCalls;
  *Descriptor=mPublishedDescriptor;return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI RamAllocate(EFI_ALLOCATE_TYPE Type,EFI_MEMORY_TYPE MemoryType,
                                     UINTN Pages,EFI_PHYSICAL_ADDRESS *Address)
{
  assert(Type==AllocateMaxAddress&&MemoryType==EfiBootServicesData&&Pages==1);
  assert(*Address>=mPublishedDescriptor.BaseAddress+EFI_PAGE_SIZE-1);
  assert(mPublishedDescriptor.Length>=EFI_PAGE_SIZE);
  ++mAllocateCalls;mLastMemoryType=MemoryType;*Address=mPublishedDescriptor.BaseAddress;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI RamFree(EFI_PHYSICAL_ADDRESS Address,UINTN Pages)
{
  assert(Address==mPublishedDescriptor.BaseAddress&&Pages==1);++mFreeCalls;
  return EFI_SUCCESS;
}

EFI_STATUS CrRamTestDmaCapabilities(UINT64 Base,UINT64 Length,UINT64 Capabilities,UINT64 Attributes)
{
  ResetState();mPublishedDescriptor=(EFI_GCD_MEMORY_SPACE_DESCRIPTOR){
    .BaseAddress=Base,.Length=Length,.Capabilities=Capabilities,.Attributes=Attributes,
    .GcdMemoryType=EfiGcdMemoryTypeSystemMemory};
  mSmmu.Device.Stage1OutputAddressBits=36;
  mBootServices.AllocatePages=RamAllocate;mBootServices.FreePages=RamFree;
  mDxeServices.GetMemorySpaceDescriptor=RamDescriptor;
  VOID *Host=(VOID *)(UINTN)0xBAD;
  EFI_STATUS Status=CrSmmuAllocateBuffer(&mIoMmuProtocol,AllocateAnyPages,
                                       EfiBootServicesData,1,&Host,0);
  if(Status!=EFI_SUCCESS){
    assert(Status==EFI_UNSUPPORTED&&Host==NULL);
    assert(mAllocateCalls==1&&mFreeCalls==1&&!mSetMemoryAttributeCalls);
    return Status;
  }
  assert(Host==(VOID *)(UINTN)Base&&mAllocateCalls==1&&!mFreeCalls);
  assert(mGetDescriptorCalls==1&&mSetMemoryAttributeCalls==1);
  assert(mLastMemoryAttributes==((Attributes&~EFI_CACHE_ATTRIBUTE_MASK)|EFI_MEMORY_WC));
  assert(mWriteBackInvalidateCalls==1);
  assert(mLastCacheAddress==Host&&mLastCacheLength==EFI_PAGE_SIZE);
  assert(CrSmmuFreeBuffer(&mIoMmuProtocol,1,Host)==EFI_SUCCESS);
  assert(mFreeCalls==1&&mLastMemoryAttributes==Attributes);
  return Status;
}
