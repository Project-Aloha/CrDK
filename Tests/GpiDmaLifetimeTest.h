/** @file IOMMU failure injection for actual GPI allocation code. SPDX-License-Identifier: MIT */
static UINT8 DmaAllocation[EFI_PAGE_SIZE];
static UINT32 DmaMapCalls, DmaUnmapCalls, DmaFreeCalls, DmaAttributeCalls;
static BOOLEAN DmaPartial, DmaFailAttribute, DmaFailUnmap, DmaFailFree;
static EFI_STATUS EFIAPI DmaAllocate (EDKII_IOMMU_PROTOCOL *This,
    EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType, UINTN Pages,
    VOID **Host, UINT64 Attributes) {
  (VOID)This; (VOID)Type; (VOID)MemoryType; (VOID)Attributes;
  assert (Pages == 1); *Host = DmaAllocation; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DmaMap (EDKII_IOMMU_PROTOCOL *This,
    EDKII_IOMMU_OPERATION Operation, VOID *Host, UINTN *Bytes,
    EFI_PHYSICAL_ADDRESS *Device, VOID **Mapping) {
  (VOID)This;
  assert (Operation == EdkiiIoMmuOperationBusMasterCommonBuffer64);
  assert (Host == DmaAllocation && *Bytes == EFI_PAGE_SIZE);
  ++DmaMapCalls;
  if (DmaPartial) --*Bytes;
  *Device = 0x400000; *Mapping = DmaAllocation; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DmaAttribute (EDKII_IOMMU_PROTOCOL *This,
    UINT64 Base, UINT32 Sid, VOID *Mapping, UINT64 Access) {
  (VOID)This;
  assert (Base == 0x15000000 && Sid == GPI_SID && Mapping == DmaAllocation);
  assert (Access == (EDKII_IOMMU_ACCESS_READ | EDKII_IOMMU_ACCESS_WRITE));
  ++DmaAttributeCalls;
  return DmaFailAttribute ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DmaUnmap (EDKII_IOMMU_PROTOCOL *This, VOID *Mapping) {
  (VOID)This; assert (Mapping == DmaAllocation); ++DmaUnmapCalls;
  return DmaFailUnmap ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EFI_STATUS EFIAPI DmaFree (EDKII_IOMMU_PROTOCOL *This, UINTN Pages, VOID *Host) {
  (VOID)This; assert (Pages == 1 && Host == DmaAllocation); ++DmaFreeCalls;
  return DmaFailFree ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
static EDKII_IOMMU_PROTOCOL DmaIommu = {
  .Revision = EDKII_IOMMU_PROTOCOL_REVISION,
  .AllocateBuffer = DmaAllocate, .Map = DmaMap, .SetAttributeById = DmaAttribute,
  .Unmap = DmaUnmap, .FreeBuffer = DmaFree
};
static UINT8 DeviceRingMemory[EFI_PAGE_SIZE];
static void PublishRingMemory (VOID *Start, UINTN Length) {
  assert (Start == DmaAllocation && Length == sizeof (DeviceRingMemory));
  memcpy (DeviceRingMemory, Start, Length);
}
static void TestRingCacheVisibility (void) {
  GPI_RING Ring = {0};
  ZeroMem (&mGpi, sizeof (mGpi));
  mGpi.Iommu = &DmaIommu;
  DmaPartial = DmaFailAttribute = DmaFailUnmap = DmaFailFree = FALSE;
  /* Model non-coherent memory: CPU stores do not reach the device until
     cache writeback. Both reused memory and its CPU copy begin nonzero. */
  memset (DmaAllocation, 0xa5, sizeof (DmaAllocation));
  memset (DeviceRingMemory, 0xa5, sizeof (DeviceRingMemory));
  TestWriteBackHook = PublishRingMemory;
  assert (GpiRingAllocate (GPI_EVENT_ELEMENTS, &Ring) == EFI_SUCCESS);
  TestWriteBackHook = NULL;
  for (UINTN Index = 0; Index < sizeof (DeviceRingMemory); ++Index)
    assert (DeviceRingMemory[Index] == 0);
  GpiRingFree (&Ring);
  assert (Ring.Host == NULL);
  puts ("GPI DMA: initialized event ring visible to non-coherent device before publication passed");
}
static void TestDmaLifetime (void) {
  TestRingCacheVisibility ();
  for (UINT32 Fault = 0; Fault < 6; ++Fault) {
    GPI_RING Ring = {0};
    ZeroMem (&mGpi, sizeof (mGpi));
    mGpi.Iommu = &DmaIommu;
    DmaMapCalls = DmaUnmapCalls = DmaFreeCalls = DmaAttributeCalls = 0;
    DmaPartial = Fault < 2;
    DmaFailAttribute = Fault == 2 || Fault == 3;
    DmaFailUnmap = Fault == 1 || Fault == 3 || Fault == 4;
    DmaFailFree = Fault == 5;
    EFI_STATUS Status = GpiRingAllocate (16, &Ring);
    if (Fault < 4) {
      assert (Status == (DmaPartial ? EFI_BAD_BUFFER_SIZE : EFI_DEVICE_ERROR));
      assert (DmaAttributeCalls == (DmaPartial ? 0U : 1U));
    } else {
      assert (Status == EFI_SUCCESS);
      GpiRingFree (&Ring);
    }
    assert (DmaUnmapCalls == 1);
    if (DmaFailUnmap || DmaFailFree) {
      assert (mGpi.DmaCleanupFailed && Ring.Host == DmaAllocation && Ring.Pages == 1);
      assert (Ring.Mapping == (DmaFailUnmap ? DmaAllocation : NULL));
      assert (DmaFreeCalls == (DmaFailUnmap ? 0U : 1U));
      assert (GpiInitialize (GPI_I2C_INSTANCE) == EFI_DEVICE_ERROR);
      assert (DmaMapCalls == 1);
      DmaFailUnmap = DmaFailFree = FALSE;
      GpiRingFree (&Ring);
      assert (Ring.Host == NULL);
    } else {
      assert (Ring.Host == NULL && DmaFreeCalls == 1);
    }
  }
  /* A retained DMA mapping also fences the QUP lease when initialization
     failed before any GPI context was programmed.  Releasing it as
     quiesced would let FIFO reuse the SE while the SMMU translation lives. */
  ZeroMem (&mGpi, sizeof (mGpi));
  mGpi.DmaCleanupFailed = TRUE;
  assert (GpiQuiesce (FALSE) == EFI_DEVICE_ERROR);
  puts ("GPI DMA: short mapping rejection, failed unmap/free retention and retry fencing passed");
}
