/* Actual non-discoverable PCI DMA -> MU IoMmuLib -> SMMU DXE/core/targets.
 * Register/cache/allocation inputs are modeled; ownership policy is real. MIT */
#include <stdlib.h>
static unsigned long long mUsbModelBase;
#include "UsbSmmuModel.inc"
#define mIoMmuProtocol mIoMmuLibProtocol
#include "../../../MU_BASECORE/MdeModulePkg/Library/IoMmuLib/IoMmuLib.c"
#undef mIoMmuProtocol
#include <Guid/NonDiscoverableDevice.h>
#include "../Override/MU_BASECORE/MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceIo.c"
static EFI_CPU_ARCH_PROTOCOL Cpu;
EFI_CPU_ARCH_PROTOCOL *mCpu=&Cpu;
VOID *EFIAPI AllocatePool(UINTN N){return malloc(N);}
VOID EFIAPI FreePool(VOID *P){free(P);}
VOID EFIAPI FreePages(VOID *P,UINTN N){(VOID)P;(VOID)N;assert(FALSE);}
LIST_ENTRY *EFIAPI RemoveEntryList(CONST LIST_ENTRY *E){E->BackLink->ForwardLink=E->ForwardLink;E->ForwardLink->BackLink=E->BackLink;return E->ForwardLink;}
LIST_ENTRY *EFIAPI InsertHeadList(LIST_ENTRY *H,LIST_ENTRY *E){E->ForwardLink=H->ForwardLink;E->BackLink=H;H->ForwardLink->BackLink=E;H->ForwardLink=E;return H;}
static EFI_STATUS EFIAPI Flush(EFI_CPU_ARCH_PROTOCOL *C,EFI_PHYSICAL_ADDRESS A,UINT64 N,EFI_CPU_FLUSH_TYPE T){assert(C==&Cpu&&A&&N&&(T==EfiCpuFlushTypeInvalidate||T==EfiCpuFlushTypeWriteBack));return EFI_SUCCESS;}
static EFI_STATUS EFIAPI SetGcd(EFI_PHYSICAL_ADDRESS A,UINT64 N,UINT64 V){EFI_STATUS S=MockSetMemorySpaceAttributes(A,N,V);if(S==EFI_SUCCESS)mDescriptorAttributes=V;return S;}
static BOOLEAN InUse(CR_SMMU_MAPPING *M){return M->InUse;}
int main(void){
 for(UINTN Board=0;Board<2;++Board)for(UINTN Port=0;Port<2;++Port){
  mUsbModelBase=Port?0xa800000:0xa600000;UINT16 Sid=Port?0x20:0;
  Fixture(Board);mSmmu.Device.Stage1OutputAddressBits=64;
  mDxeServices.SetMemorySpaceAttributes=SetGcd;
  Hardware[(0x800+3*4)/4]=0x80000000U|Sid;
  assert(CrSmmuAcquireUsbStream(&mCrProtocol,mUsbModelBase,Sid,NativeStopped,Usb)==EFI_SUCCESS);
  CR_SMMU_DOMAIN *D=FindDomainBySid(Sid);assert(D&&D->UsbOwner==Usb&&D->UsbStopped==NativeStopped);
  /* A second actual USB owner proves grant isolation across live domains. */
  UINTN SelectedBase=mUsbModelBase;mUsbModelBase=Port?0xa600000:0xa800000;
  assert(CrSmmuAcquireUsbStream(&mCrProtocol,mUsbModelBase,Sid^0x20,NativeStopped,Usb)==EFI_SUCCESS);
  mUsbModelBase=SelectedBase;
  UINTN StopsBeforeMap=StopChecks;
  Usb[0x40/4]=1;Usb[0x44/4]=0; /* published HCI running: no stopped recheck at Map */
  mIoMmuProtocol.Revision=EDKII_IOMMU_PROTOCOL_REVISION;
  mIoMmuProtocol.Map=CrSmmuMap;mIoMmuProtocol.Unmap=CrSmmuUnmap;
  mIoMmuProtocol.SetAttribute=CrSmmuSetAttribute;mIoMmuProtocol.AllocateBuffer=CrSmmuAllocateBuffer;
  mIoMmuProtocol.FreeBuffer=CrSmmuFreeBuffer;mIoMmuLibProtocol=&mIoMmuProtocol;
  EFI_DMA_IDENTITY_CR_PROTOCOL Identity={EFI_DMA_IDENTITY_CR_PROTOCOL_REVISION,HW_BASE,mUsbModelBase,0xcd00,Sid};mIdentity=&Identity;
  Cpu.DmaBufferAlignment=64;Cpu.FlushDataCache=Flush;
  NON_DISCOVERABLE_PCI_DEVICE Dev={.Signature=NON_DISCOVERABLE_PCI_DEVICE_SIG,.Attributes=EFI_PCI_IO_ATTRIBUTE_DUAL_ADDRESS_CYCLE,.Handle=(EFI_HANDLE)(UINTN)0xd0};
  Dev.UncachedAllocationList.ForwardLink=Dev.UncachedAllocationList.BackLink=&Dev.UncachedAllocationList;
  VOID *Buffer,*Token=NULL;UINTN Bytes=4096;EFI_PHYSICAL_ADDRESS Iova;
  assert(NonCoherentPciIoAllocateBuffer(&Dev.PciIo,AllocateAnyPages,EfiBootServicesData,1,&Buffer,0)==EFI_SUCCESS);
  mDescriptorAttributes=mLastMemoryAttributes;assert((mDescriptorAttributes&EFI_CACHE_ATTRIBUTE_MASK)==EFI_MEMORY_WC);
  assert(NonCoherentPciIoMap(&Dev.PciIo,EfiPciIoOperationBusMasterCommonBuffer,Buffer,&Bytes,&Iova,&Token)==EFI_SUCCESS);
  assert(StopChecks==StopsBeforeMap);
  assert(Token&&Iova>=SMMU_IOVA_BASE);NON_DISCOVERABLE_PCI_DEVICE_MAP_INFO *N=Token;
  CR_SMMU_MAPPING *M=FindMapping(N->IoMmuContext);assert(M&&M->DomainAccess[D-mSmmu.Domains]==3);
  SmmuMappingEvidence E;assert(SmmuQueryMappingEvidence(&D->Domain,M->IovaBase,M->PhysicalBase,M->Pages,3,&E)==CR_SUCCESS);
  for(UINTN I=0;I<ARRAY_SIZE(mSmmu.Domains);++I)if(mSmmu.Domains[I].InUse&&&mSmmu.Domains[I]!=D)assert(!M->DomainAccess[I]);
  UINTN W=HardwareWrites;
  Identity.Revision++;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_ACCESS_DENIED&&HardwareWrites==W);Identity.Revision--;
  Identity.ControllerSize=0xc000;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_ACCESS_DENIED&&HardwareWrites==W);Identity.ControllerSize=0xcd00;
  Identity.ControllerBase^=0x1000;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_ACCESS_DENIED&&HardwareWrites==W);Identity.ControllerBase^=0x1000;
  Identity.Sid^=1;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_ACCESS_DENIED&&HardwareWrites==W);Identity.Sid=Sid;
  Identity.IommuBase^=0x1000;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_ACCESS_DENIED&&HardwareWrites==W);Identity.IommuBase=HW_BASE;
  D->UsbOwner=NULL;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_NOT_READY&&HardwareWrites==W);D->UsbOwner=Usb;
  D->UsbStopped=NULL;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_NOT_READY&&HardwareWrites==W);D->UsbStopped=NativeStopped;
  D->Bdf=0;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_NOT_READY&&HardwareWrites==W);D->Bdf=0xffff;
  D->Segment=1;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,0)==EFI_NOT_READY&&HardwareWrites==W);D->Segment=0;
  assert(CrSmmuSetAttribute(&mIoMmuProtocol,(EFI_HANDLE)(UINTN)0xdead,M,0)==EFI_UNSUPPORTED&&HardwareWrites==W);
  assert(NonCoherentPciIoUnmap(&Dev.PciIo,Token)==EFI_SUCCESS&&!InUse(M));
  assert(NonCoherentPciIoFreeBuffer(&Dev.PciIo,1,Buffer)==EFI_SUCCESS);
  Bytes=64;VOID *R=mPagePool+200*4096;
  assert(NonCoherentPciIoMap(&Dev.PciIo,EfiPciIoOperationBusMasterRead,R,&Bytes,&Iova,&Token)==EFI_SUCCESS);
  N=Token;M=FindMapping(N->IoMmuContext);assert(M&&M->DomainAccess[D-mSmmu.Domains]==1);
  W=HardwareWrites;assert(CrSmmuSetAttribute(&mIoMmuProtocol,Dev.Handle,M,2)==EFI_UNSUPPORTED&&HardwareWrites==W);
  assert(NonCoherentPciIoUnmap(&Dev.PciIo,Token)==EFI_SUCCESS);
  mIdentity=NULL;
 }
 puts("Actual native USB DMA chain: both SM8250/8350 ports, controller Handle/native identity, exact owned SID domain/common+read DMA, tuple/owner/permission fences PASS");return 0;
}
