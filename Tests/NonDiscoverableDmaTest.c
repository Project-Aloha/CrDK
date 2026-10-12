/** Exercise real PCI emulation allocation/Map/Unmap ownership.
    SPDX-License-Identifier: MIT */
#include <Uefi.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <Guid/NonDiscoverableDevice.h>
#include "../Override/MU_BASECORE/MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceIo.c"

static EFI_BOOT_SERVICES Services;
EFI_BOOT_SERVICES *gBS=&Services;
static EFI_DXE_SERVICES DxeServices;
EFI_DXE_SERVICES *gDS=&DxeServices;
static EFI_CPU_ARCH_PROTOCOL Cpu;
EFI_CPU_ARCH_PROTOCOL *mCpu=&Cpu;
static VOID *Allocation;
static UINTN AllocationPages, MapCount, ExpectedAsserts;
static EFI_STATUS FreeStatus, AttributeStatus, MapStatus, UnmapStatus;
static UINT64 MemoryAttributes;

BOOLEAN EFIAPI DebugPrintEnabled(VOID) { return FALSE; }
BOOLEAN EFIAPI DebugPrintLevelEnabled(CONST UINTN L) { (VOID)L; return FALSE; }
BOOLEAN EFIAPI DebugAssertEnabled(VOID) { return TRUE; }
VOID EFIAPI DebugPrint(UINTN L, CONST CHAR8 *F, ...) { (VOID)L; (VOID)F; }
VOID EFIAPI DebugAssert(CONST CHAR8 *F, UINTN L, CONST CHAR8 *D)
{
  if(ExpectedAsserts && strstr(F,"NonDiscoverablePciDeviceIo.c")) {
    --ExpectedAsserts;
    return;
  }
  fprintf(stderr,"%s:%lu %s\n",F,(unsigned long)L,D);
  abort();
}
VOID *EFIAPI AllocatePool(UINTN N) { return malloc(N); }
VOID EFIAPI FreePool(VOID *P) { free(P); }
VOID *EFIAPI CopyMem(VOID *D, CONST VOID *S, UINTN N) { return memcpy(D,S,N); }
VOID *EFIAPI ZeroMem(VOID *D, UINTN N) { return memset(D,0,N); }
VOID EFIAPI FreePages(VOID *P, UINTN N) { (VOID)P; (VOID)N; assert(FALSE); }
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *E)
{ E->BackLink->ForwardLink=E->ForwardLink; E->ForwardLink->BackLink=E->BackLink; return E->ForwardLink; }
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *H, LIST_ENTRY *E)
{ E->ForwardLink=H->ForwardLink; E->BackLink=H; H->ForwardLink->BackLink=E; H->ForwardLink=E; return H; }
EFI_STATUS EFIAPI IoMmuAllocateBuffer(EFI_ALLOCATE_TYPE T, EFI_MEMORY_TYPE M, UINTN N, VOID **P, UINT64 A)
{
  assert(T==AllocateAnyPages && M==EfiBootServicesData && N && Allocation==NULL);
  assert(A==EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE);
  *P=Allocation=aligned_alloc(4096,N*4096); assert(Allocation); AllocationPages=N;
  MemoryAttributes=EFI_MEMORY_WC; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI IoMmuFreeBuffer(UINTN N, VOID *P)
{
  assert(P==Allocation && N==AllocationPages);
  if(!EFI_ERROR(FreeStatus)) { assert(MapCount==0); free(P); Allocation=NULL; }
  return FreeStatus;
}
EFI_STATUS EFIAPI IoMmuMap(EDKII_IOMMU_OPERATION O, VOID *P, UINTN *N, EFI_PHYSICAL_ADDRESS *D, VOID **T)
{
  assert(O==EdkiiIoMmuOperationBusMasterCommonBuffer64 && P==Allocation && *N==4096 &&
         (MemoryAttributes&EFI_CACHE_ATTRIBUTE_MASK)==EFI_MEMORY_WC);
  if(EFI_ERROR(MapStatus)) return MapStatus;
  *T=P; *D=0x10000000; ++MapCount; return EFI_SUCCESS;
}
EFI_STATUS EFIAPI IoMmuUnmap(VOID *T)
{
  assert(T==Allocation && MapCount);
  if(EFI_ERROR(UnmapStatus)) return UnmapStatus;
  --MapCount;
  return EFI_SUCCESS;
}
EFI_STATUS EFIAPI IoMmuSetAttribute(EFI_HANDLE H, VOID *T, UINT64 A)
{ assert(H==(EFI_HANDLE)0xD0 && T==Allocation && (A==0 || A==3)); return AttributeStatus; }
static EFI_STATUS EFIAPI Descriptor(EFI_PHYSICAL_ADDRESS A, EFI_GCD_MEMORY_SPACE_DESCRIPTOR *D)
{ assert(A==(UINTN)Allocation); memset(D,0,sizeof(*D)); D->Attributes=MemoryAttributes; D->Capabilities=EFI_MEMORY_WC|EFI_MEMORY_UC|EFI_MEMORY_XP; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Attributes(EFI_PHYSICAL_ADDRESS A, UINT64 N, UINT64 V)
{ assert(A==(UINTN)Allocation && N==AllocationPages*4096); assert((V&EFI_MEMORY_UC)==0); MemoryAttributes=V; return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Flush(EFI_CPU_ARCH_PROTOCOL *C, EFI_PHYSICAL_ADDRESS A, UINT64 N, EFI_CPU_FLUSH_TYPE T)
{ assert(C==&Cpu && A==(UINTN)Allocation && N==AllocationPages*4096 &&
         (T==EfiCpuFlushTypeInvalidate || T==EfiCpuFlushTypeWriteBack)); return EFI_SUCCESS; }
int main(void)
{
  NON_DISCOVERABLE_PCI_DEVICE Dev={.Signature=NON_DISCOVERABLE_PCI_DEVICE_SIG,
      .Attributes=EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,.Handle=(EFI_HANDLE)0xD0};
  Dev.UncachedAllocationList.ForwardLink=Dev.UncachedAllocationList.BackLink=&Dev.UncachedAllocationList;
  DxeServices.GetMemorySpaceDescriptor=Descriptor; DxeServices.SetMemorySpaceAttributes=Attributes;
  Cpu.FlushDataCache=Flush; Cpu.DmaBufferAlignment=64;
  VOID *CpuAddress, *Token; EFI_PHYSICAL_ADDRESS Device; UINTN Bytes=4096;
  EFI_PCI_IO_PROTOCOL_MAP Map[]={CoherentPciIoMap,NonCoherentPciIoMap};
  EFI_PCI_IO_PROTOCOL_UNMAP Unmap[]={CoherentPciIoUnmap,NonCoherentPciIoUnmap};
  assert(NonCoherentPciIoAllocateBuffer(&Dev.PciIo,AllocateAnyPages,EfiBootServicesData,1,&CpuAddress,0)==EFI_SUCCESS);
  assert(CpuAddress==Allocation && (MemoryAttributes&EFI_MEMORY_WC));
  for(UINTN I=0; I<ARRAY_SIZE(Map); ++I) {
    MapStatus=EFI_OUT_OF_RESOURCES; ExpectedAsserts=1; Token=(VOID *)1;
    assert(Map[I](&Dev.PciIo,EfiPciIoOperationBusMasterCommonBuffer,CpuAddress,&Bytes,&Device,&Token)==MapStatus);
    assert(MapCount==0 && Token==NULL && ExpectedAsserts==0);
    MapStatus=EFI_SUCCESS;
    AttributeStatus=EFI_ACCESS_DENIED; ExpectedAsserts=1;
    assert(Map[I](&Dev.PciIo,EfiPciIoOperationBusMasterCommonBuffer,CpuAddress,&Bytes,&Device,&Token)==AttributeStatus);
    assert(MapCount==0 && Token==NULL && ExpectedAsserts==0);

    UnmapStatus=EFI_DEVICE_ERROR; ExpectedAsserts=2;
    assert(Map[I](&Dev.PciIo,EfiPciIoOperationBusMasterCommonBuffer,CpuAddress,&Bytes,&Device,&Token)==AttributeStatus);
    assert(MapCount==1 && Token!=NULL && ExpectedAsserts==0);
    AttributeStatus=UnmapStatus=EFI_SUCCESS;
    assert(Unmap[I](&Dev.PciIo,Token)==EFI_SUCCESS && MapCount==0);

    assert(Map[I](&Dev.PciIo,EfiPciIoOperationBusMasterCommonBuffer,CpuAddress,&Bytes,&Device,&Token)==EFI_SUCCESS);
    assert(Device==0x10000000 && MapCount==1);
    AttributeStatus=EFI_DEVICE_ERROR; ExpectedAsserts=1;
    assert(Unmap[I](&Dev.PciIo,Token)==EFI_DEVICE_ERROR && MapCount==1 && ExpectedAsserts==0);
    AttributeStatus=EFI_SUCCESS; UnmapStatus=EFI_TIMEOUT; ExpectedAsserts=1;
    assert(Unmap[I](&Dev.PciIo,Token)==EFI_TIMEOUT && MapCount==1 && ExpectedAsserts==0);
    UnmapStatus=EFI_SUCCESS;
    assert(Unmap[I](&Dev.PciIo,Token)==EFI_SUCCESS && MapCount==0);
  }
  FreeStatus=EFI_ACCESS_DENIED;
  assert(NonCoherentPciIoFreeBuffer(&Dev.PciIo,1,CpuAddress)==EFI_ACCESS_DENIED && Allocation!=NULL);
  assert(Dev.UncachedAllocationList.ForwardLink!=&Dev.UncachedAllocationList);
  FreeStatus=EFI_SUCCESS;
  assert(NonCoherentPciIoFreeBuffer(&Dev.PciIo,1,CpuAddress)==EFI_SUCCESS && Allocation==NULL);
  assert(Dev.UncachedAllocationList.ForwardLink==&Dev.UncachedAllocationList);
  assert(NonCoherentPciIoAllocateBuffer(&Dev.PciIo,AllocateAnyPages,EfiBootServicesData,1,&CpuAddress,
         EFI_PCI_ATTRIBUTE_MEMORY_WRITE_COMBINE)==EFI_SUCCESS);
  assert(NonCoherentPciIoFreeBuffer(&Dev.PciIo,1,CpuAddress)==EFI_SUCCESS && Allocation==NULL);
  puts("NonDiscoverable PCI DMA: IOMMU allocation, WC, controller identity and retry ownership passed");
  return 0;
}
