/* Reserved USBH record and ordered native USB/SMMU retirement. MIT */
#include <PiDxe.h>
#include "Dwc3Handoff.h"
#include <Guid/EventGroup.h>
#include <IndustryStandard/Acpi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/AcpiTable.h>

#pragma pack(1)
typedef struct {
  EFI_ACPI_DESCRIPTION_HEADER Header;
  UINT64 Record;
  UINT32 Length,Reserved;
  UINT64 Generation;
} CR_USB_NATIVE_LOCATOR;
#pragma pack()
STATIC_ASSERT(sizeof(CR_USB_NATIVE_LOCATOR)==60,"USBH ACPI size");
STATIC_ASSERT(OFFSET_OF(CR_USB_NATIVE_LOCATOR,Record)==36,"USBH PA offset");
STATIC_ASSERT(OFFSET_OF(CR_USB_NATIVE_LOCATOR,Generation)==52,"USBH generation offset");

STATIC struct {
  EFI_SMMU_CR_PROTOCOL *Smmu;
  CR_USB_NATIVE_FREEZE Freeze;
  EFI_ACPI_TABLE_PROTOCOL *Acpi;
  EFI_PHYSICAL_ADDRESS Page;
  EFI_MEMORY_DESCRIPTOR *MemoryMap;
  UINTN MapCapacity,TableKey;
  UINT64 Generation,Transfer;
  EFI_EVENT AcpiEvent,EndEvent,ReadyEvent;
  VOID *Registration;
  BOOLEAN PageOwned,TableOwned,Installed,Preparing,PrepareObserver,CompleteObserver;
  BOOLEAN Frozen,Freezing,Complete,Published,Closing;
  CR_USB_NATIVE_HANDOFF Local;
} mUsbHandoff;

STATIC EFI_STATUS Exact(EFI_STATUS S) {return S==EFI_SUCCESS||EFI_ERROR(S)?S:EFI_NOT_READY;}
STATIC BOOLEAN Open(VOID) {return !mUsbHandoff.Frozen&&!mUsbHandoff.Freezing&&!mUsbHandoff.Complete&&!mUsbHandoff.Closing;}
STATIC BOOLEAN Covers(UINT64 B,UINT64 N,UINT64 P,UINT64 L) {
  return N&&L&&B<=MAX_UINT64-N&&P>=B&&L<=N&&P-B<=N-L;
}
STATIC EFI_STATUS NormalRam(UINT64 P,UINT64 N) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS S=gDS->GetMemorySpaceDescriptor(P,&D);
  if(S!=EFI_SUCCESS)return Exact(S);
  if(!Covers(D.BaseAddress,D.Length,P,N)||
     (D.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory&&D.GcdMemoryType!=EfiGcdMemoryTypeReserved)||
     (D.Attributes&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO|EFI_MEMORY_UC|EFI_MEMORY_UCE))||
     !(D.Attributes&(EFI_MEMORY_WB|EFI_MEMORY_WT|EFI_MEMORY_WC)))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Count(UINT64 *Value) {
  for(UINTN I=0;I<2&&!*Value;++I){
    EFI_STATUS S=gBS->GetNextMonotonicCount(Value);
    if(S!=EFI_SUCCESS){*Value=0;return Exact(S);}
  }
  return *Value?EFI_SUCCESS:EFI_NOT_READY;
}
STATIC EFI_STATUS RemoveLocator(VOID) {
  if(!mUsbHandoff.TableOwned)return EFI_SUCCESS;
  // MU may commit AddTableToList before PublishTables reports an error. Keep
  // that key and its backing page until removal is positively acknowledged.
  // NOT_FOUND is ambiguous here: MU also returns it on a publication failure.
  mUsbHandoff.Installed=FALSE;
  EFI_STATUS S=mUsbHandoff.Acpi->UninstallAcpiTable(mUsbHandoff.Acpi,mUsbHandoff.TableKey);
  if(S!=EFI_SUCCESS)return Exact(S);
  mUsbHandoff.TableOwned=FALSE;mUsbHandoff.TableKey=0;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS PrepareStorage(VOID) {
  EFI_STATUS S;UINTN Size,Key,Stride;UINT32 Version;
  CR_USB_NATIVE_LOCATOR L={0};
  if(!Open())return EFI_ACCESS_DENIED;
  if(mUsbHandoff.Installed)return EFI_SUCCESS;
  S=RemoveLocator();if(S!=EFI_SUCCESS)return S;
  if(!Open())return EFI_ACCESS_DENIED;
  if(!mUsbHandoff.Acpi){
    EFI_ACPI_TABLE_PROTOCOL *A=NULL;
    S=gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid,NULL,(VOID**)&A);
    if(S!=EFI_SUCCESS)return Exact(S);
    if(!A||!A->InstallAcpiTable||!A->UninstallAcpiTable)return EFI_UNSUPPORTED;
    mUsbHandoff.Acpi=A;
  }
  if(!Open())return EFI_ACCESS_DENIED;
  S=Count(&mUsbHandoff.Generation);if(S!=EFI_SUCCESS)return S;
  if(!Open())return EFI_ACCESS_DENIED;
  S=Count(&mUsbHandoff.Transfer);if(S!=EFI_SUCCESS)return S;
  if(!Open())return EFI_ACCESS_DENIED;
  if(!mUsbHandoff.PageOwned){
    S=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,1,&mUsbHandoff.Page);
    if(S!=EFI_SUCCESS){mUsbHandoff.Page=0;return Exact(S);}
    mUsbHandoff.PageOwned=TRUE;
  }
  if(!Open())return EFI_ACCESS_DENIED;
  if(mUsbHandoff.Page<0x100000||(mUsbHandoff.Page&EFI_PAGE_MASK))return EFI_UNSUPPORTED;
  S=NormalRam(mUsbHandoff.Page,EFI_PAGE_SIZE);if(S!=EFI_SUCCESS)return S;
  if(!Open())return EFI_ACCESS_DENIED;
  ZeroMem((VOID*)(UINTN)mUsbHandoff.Page,EFI_PAGE_SIZE);
  WriteBackDataCacheRange((VOID*)(UINTN)mUsbHandoff.Page,EFI_PAGE_SIZE);MemoryFence();
  if(!mUsbHandoff.MemoryMap){
    Size=Stride=0;S=gBS->GetMemoryMap(&Size,NULL,&Key,&Stride,&Version);
    if(S!=EFI_BUFFER_TOO_SMALL)return EFI_ERROR(S)?S:EFI_COMPROMISED_DATA;
    if(!Open())return EFI_ACCESS_DENIED;
    if(Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>1024||Size>1024*1024-128*Stride)
      return EFI_OUT_OF_RESOURCES;
    Size+=128*Stride;if(Size<65536)Size=65536;
    S=gBS->AllocatePool(EfiBootServicesData,Size,(VOID**)&mUsbHandoff.MemoryMap);
    if(S!=EFI_SUCCESS){mUsbHandoff.MemoryMap=NULL;return Exact(S);}
    mUsbHandoff.MapCapacity=Size;
  }
  if(!Open())return EFI_ACCESS_DENIED;
  L.Header.Signature=CR_USBH_SIGNATURE;L.Header.Length=sizeof(L);L.Header.Revision=1;
  CopyMem(L.Header.OemId,"ALOHA ",6);CopyMem(&L.Header.OemTableId,"USBHDK84",8);
  L.Header.OemRevision=1;L.Header.CreatorId=SIGNATURE_32('C','R','D','K');L.Header.CreatorRevision=1;
  L.Record=mUsbHandoff.Page;L.Length=sizeof(CR_USB_NATIVE_HANDOFF);L.Generation=mUsbHandoff.Generation;
  L.Header.Checksum=CalculateCheckSum8((UINT8*)&L,sizeof(L));
  mUsbHandoff.TableKey=0;
  S=mUsbHandoff.Acpi->InstallAcpiTable(mUsbHandoff.Acpi,&L,sizeof(L),&mUsbHandoff.TableKey);
  mUsbHandoff.TableOwned=(S==EFI_SUCCESS||mUsbHandoff.TableKey!=0);
  if(S!=EFI_SUCCESS)return Exact(S);
  mUsbHandoff.Installed=TRUE;
  return Open()?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
STATIC VOID EFIAPI Prepare(EFI_EVENT Event,VOID *Context) {
  (VOID)Event;(VOID)Context;
  if(mUsbHandoff.Preparing||!Open()||mUsbHandoff.Installed)return;
  mUsbHandoff.Preparing=TRUE;
  EFI_STATUS S=PrepareStorage();
  mUsbHandoff.Preparing=FALSE;
  DEBUG((EFI_ERROR(S)?DEBUG_WARN:DEBUG_INFO,"USBH: reserved=%Lx generation=%Lu prepare: %r\n",
    mUsbHandoff.Page,mUsbHandoff.Generation,S));
}
STATIC EFI_STATUS EFIAPI BeforeRevoke(VOID *Context) {
  if(Context!=&mUsbHandoff)return EFI_INVALID_PARAMETER;
  if(mUsbHandoff.Frozen)return (EFI_STATUS)mUsbHandoff.Local.FreezeStatus;
  if(mUsbHandoff.Freezing)return EFI_NOT_READY;
  mUsbHandoff.Freezing=TRUE;
  CR_USB_NATIVE_HANDOFF *R=&mUsbHandoff.Local;
  ZeroMem(R,sizeof(*R));
  R->Signature=CR_USBH_SIGNATURE;R->Revision=CR_USBH_REVISION;R->Length=sizeof(*R);
  R->BootGeneration=mUsbHandoff.Generation;R->TransferId=mUsbHandoff.Transfer;
  R->Soc=0x8450;R->Board=1;R->Controller=CR_USBH_CONTROLLER;R->ControllerBytes=CR_USBH_CONTROLLER_BYTES;
  R->Qscratch=CR_USBH_QSCRATCH;R->QscratchBytes=CR_USBH_QSCRATCH_BYTES;
  R->ExpectedWriters=CR_USBH_ALL_WRITERS;R->State=CR_USBH_UNCERTAIN;
  R->FreezeStatus=R->HaltStatus=R->InterruptStatus=R->RevocationStatus=EFI_NOT_READY;
  EFI_STATUS S=mUsbHandoff.Freeze?mUsbHandoff.Freeze(R):EFI_NOT_READY;
  R->FreezeStatus=S;
  if(mUsbHandoff.Preparing && S==EFI_SUCCESS)R->FreezeStatus=EFI_NOT_READY;
  mUsbHandoff.Frozen=TRUE;mUsbHandoff.Freezing=FALSE;
  return Exact((EFI_STATUS)R->FreezeStatus);
}
STATIC EFI_STATUS Reserved(UINTN Size,UINTN Stride,UINT64 P,UINT64 N) {
  BOOLEAN Found=FALSE;
  if(!P||!N||P>MAX_UINT64-N)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<Size;I+=Stride){
    EFI_MEMORY_DESCRIPTOR D;CopyMem(&D,(UINT8*)mUsbHandoff.MemoryMap+I,sizeof(D));
    if(!D.NumberOfPages||D.NumberOfPages>MAX_UINT64/EFI_PAGE_SIZE||(D.PhysicalStart&EFI_PAGE_MASK))
      return EFI_COMPROMISED_DATA;
    UINT64 Bytes=D.NumberOfPages*EFI_PAGE_SIZE;
    if(D.PhysicalStart>MAX_UINT64-Bytes)return EFI_COMPROMISED_DATA;
    if(D.PhysicalStart<P+N && P<D.PhysicalStart+Bytes){
      if(Found||D.Type!=EfiReservedMemoryType||!Covers(D.PhysicalStart,Bytes,P,N))return EFI_ACCESS_DENIED;
      Found=TRUE;
    }
  }
  return Found?NormalRam(P,N):EFI_NOT_FOUND;
}
STATIC EFI_STATUS Publish(VOID) {
  if(mUsbHandoff.Published)return EFI_SUCCESS;
  if(!mUsbHandoff.Complete||!mUsbHandoff.Frozen||!mUsbHandoff.Installed||
     !mUsbHandoff.PageOwned||!mUsbHandoff.MemoryMap)return EFI_NOT_READY;
  UINTN Size=mUsbHandoff.MapCapacity,Key,Stride;UINT32 Version;
  EFI_STATUS S=gBS->GetMemoryMap(&Size,mUsbHandoff.MemoryMap,&Key,&Stride,&Version);
  if(S!=EFI_SUCCESS)return Exact(S);
  if(Version!=EFI_MEMORY_DESCRIPTOR_VERSION||Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>1024||
     (Stride&7)||Size>mUsbHandoff.MapCapacity||Size%Stride)return EFI_COMPROMISED_DATA;
  S=Reserved(Size,Stride,mUsbHandoff.Page,EFI_PAGE_SIZE);if(S!=EFI_SUCCESS)return S;
  CR_USB_NATIVE_HANDOFF *R=&mUsbHandoff.Local;
  if(R->RevocationStatus==EFI_SUCCESS){
    S=Reserved(Size,Stride,R->Revoked.Tables,R->Revoked.TableBytes);
    if(S==EFI_SUCCESS && R->Revoked.Tables<mUsbHandoff.Page+EFI_PAGE_SIZE &&
       mUsbHandoff.Page<R->Revoked.Tables+R->Revoked.TableBytes)S=EFI_ACCESS_DENIED;
    if(S!=EFI_SUCCESS)R->RevocationStatus=S;
  }
  R->State=CR_USBH_RETIRED;
  S=(EFI_STATUS)CrUsbHandoffSeal(R,2);
  if(S!=EFI_SUCCESS){R->State=CR_USBH_UNCERTAIN;S=(EFI_STATUS)CrUsbHandoffSeal(R,2);}
  if(S!=EFI_SUCCESS)return S;
  volatile CR_USB_NATIVE_HANDOFF *Shared=(VOID*)(UINTN)mUsbHandoff.Page;
  Shared->Sequence=1;WriteBackDataCacheRange((VOID*)&Shared->Sequence,sizeof(UINT64));MemoryFence();
  CopyMem((VOID*)Shared,R,OFFSET_OF(CR_USB_NATIVE_HANDOFF,Sequence));
  CopyMem((UINT8*)(UINTN)mUsbHandoff.Page+24,(UINT8*)R+24,sizeof(*R)-24);
  WriteBackDataCacheRange((VOID*)Shared,sizeof(*R));MemoryFence();
  Shared->Sequence=2;WriteBackDataCacheRange((VOID*)&Shared->Sequence,sizeof(UINT64));MemoryFence();
  mUsbHandoff.Published=TRUE;
  return EFI_SUCCESS;
}
STATIC VOID LegacyUsbPhysicalHandoff(VOID) {
  CR_USB_NATIVE_HANDOFF Proof=mUsbHandoff.Local;
  if(!FixedPcdGetBool(PcdUsbLegacyPhysicalDmaHandoff) ||
     !mUsbHandoff.Frozen || !mUsbHandoff.Complete || mUsbHandoff.Preparing ||
     Proof.Stop.Role!=CR_USBH_FUNCTION ||
     mUsbHandoff.Smmu->Revision<EFI_SMMU_CR_EXIT_USB_PHYSICAL_REVISION ||
     !mUsbHandoff.Smmu->ReleaseUsbToPhysicalAtExit)return;
  // Use the same full software/hardware/whole-domain retirement checks as
  // USBH takeover before granting legacy physical DMA. This is a local copy,
  // never a published false revocation record.
  Proof.State=CR_USBH_RETIRED;
  EFI_STATUS ProofStatus=(EFI_STATUS)CrUsbHandoffSeal(&Proof,2);
  if(ProofStatus!=EFI_SUCCESS){
    DEBUG((DEBUG_WARN,"USBH: legacy physical DMA handoff skipped: retirement proof %r\n",ProofStatus));
    return;
  }
  // Invalidate the claim before any possibly committed physical transition.
  // Both failure and success must remain unavailable to the CR Windows path.
  mUsbHandoff.Local.RevocationStatus=EFI_UNSUPPORTED;
  ZeroMem(&mUsbHandoff.Local.Revoked,sizeof(mUsbHandoff.Local.Revoked));
  EFI_STATUS S=mUsbHandoff.Smmu->ReleaseUsbToPhysicalAtExit(mUsbHandoff.Smmu);
  DEBUG((EFI_ERROR(S)?DEBUG_ERROR:DEBUG_WARN,
    "USBH: legacy device-mode physical DMA handoff: %r; CR takeover unavailable\n",S));
}
STATIC VOID EFIAPI AfterRevoke(VOID *Context,EFI_STATUS Status) {
  (VOID)Status;
  if(Context!=&mUsbHandoff||mUsbHandoff.Complete)return;
  BOOLEAN Prepared=mUsbHandoff.Frozen;
  if(!Prepared){(VOID)BeforeRevoke(Context);mUsbHandoff.Local.FreezeStatus=EFI_NOT_READY;}
  mUsbHandoff.Complete=TRUE;
  mUsbHandoff.Local.RevocationStatus=mUsbHandoff.Smmu->QueryExitRevokedDomain(
    mUsbHandoff.Smmu,0,0xffff,0,&mUsbHandoff.Local.Revoked);
  LegacyUsbPhysicalHandoff();
  EFI_STATUS S=Publish();
  // Emit this one-shot handoff summary at the board's normal DEBUG level.
  // A successful halt alone does not establish DMA retirement or publication.
  DEBUG((EFI_ERROR(S)?DEBUG_ERROR:DEBUG_WARN,
    "USBH: state=%u writers=%x/%x active=%u/%u/%u freeze=%r halt=%r irq=%r revoke=%r publish=%r\n",
    mUsbHandoff.Local.State,mUsbHandoff.Local.FrozenWriters,mUsbHandoff.Local.DrainedWriters,
    mUsbHandoff.Local.ActiveCalls[0],mUsbHandoff.Local.ActiveCalls[1],mUsbHandoff.Local.ActiveCalls[2],
    (EFI_STATUS)mUsbHandoff.Local.FreezeStatus,(EFI_STATUS)mUsbHandoff.Local.HaltStatus,
    (EFI_STATUS)mUsbHandoff.Local.InterruptStatus,(EFI_STATUS)mUsbHandoff.Local.RevocationStatus,S));
  CONST CR_USB_CONTROLLER_STOP *Stop=&mUsbHandoff.Local.Stop;
  DEBUG((DEBUG_WARN,
    "USBH: stopped role=%u GCTL=%08x host=%08x/%08x device=%08x/%08x events=%u/%u\n",
    Stop->Role,Stop->GlobalControl,Stop->HostCommand,Stop->HostStatus,
    Stop->DeviceControl,Stop->DeviceStatus,Stop->MaskedEventBuffers,Stop->EventBuffers));
  if(mUsbHandoff.Local.RevocationStatus==EFI_SUCCESS) {
    CONST SmmuMappingEvidence *D=&mUsbHandoff.Local.Revoked;
    DEBUG((DEBUG_WARN,
      "USBH: revoked SID=%x SMR[%u]=%08x S2CR=%08x CB=%u SCTLR=%08x TTBR0=%Lx IOVA=%Lx bytes=%Lx\n",
      D->Sid,D->Stream,D->Smr,D->S2cr,D->Bank,D->Sctlr,D->Ttbr0,D->Iova,D->Bytes));
  }
}
VOID CrUsbNativeExit(VOID) {(VOID)BeforeRevoke(&mUsbHandoff);if(mUsbHandoff.Complete)(VOID)Publish();}
EFI_STATUS CrUsbNativeRelease(VOID) {
  EFI_STATUS S;
  if(mUsbHandoff.Frozen||mUsbHandoff.Freezing||mUsbHandoff.Complete||mUsbHandoff.Preparing)return EFI_ACCESS_DENIED;
  mUsbHandoff.Closing=TRUE;
  if(mUsbHandoff.PrepareObserver){
    S=mUsbHandoff.Smmu->SetExitPrepareObserver(mUsbHandoff.Smmu,&mUsbHandoff,NULL,NULL);
    if(S!=EFI_SUCCESS)return Exact(S);mUsbHandoff.PrepareObserver=FALSE;
  }
  if(mUsbHandoff.CompleteObserver){
    S=mUsbHandoff.Smmu->SetExitObserver(mUsbHandoff.Smmu,&mUsbHandoff,NULL,NULL);
    if(S!=EFI_SUCCESS)return Exact(S);mUsbHandoff.CompleteObserver=FALSE;
  }
  EFI_EVENT *Events[]={&mUsbHandoff.AcpiEvent,&mUsbHandoff.EndEvent,&mUsbHandoff.ReadyEvent};
  for(UINTN I=0;I<ARRAY_SIZE(Events);++I)if(*Events[I]){
    S=gBS->CloseEvent(*Events[I]);if(S!=EFI_SUCCESS)return Exact(S);*Events[I]=NULL;
  }
  S=RemoveLocator();if(S!=EFI_SUCCESS)return S;
  if(mUsbHandoff.MemoryMap){S=gBS->FreePool(mUsbHandoff.MemoryMap);if(S!=EFI_SUCCESS)return Exact(S);mUsbHandoff.MemoryMap=NULL;}
  if(mUsbHandoff.PageOwned){S=gBS->FreePages(mUsbHandoff.Page,1);if(S!=EFI_SUCCESS)return Exact(S);mUsbHandoff.PageOwned=FALSE;}
  ZeroMem(&mUsbHandoff,sizeof(mUsbHandoff));return EFI_SUCCESS;
}
EFI_STATUS CrUsbNativeInit(EFI_SMMU_CR_PROTOCOL *Smmu,CR_USB_NATIVE_FREEZE Freeze) {
  EFI_STATUS S;
  if(!Smmu||Smmu->Revision<EFI_SMMU_CR_EXIT_DOMAIN_REVISION||!Smmu->QueryExitRevokedDomain||
     !Smmu->SetExitPrepareObserver||!Smmu->SetExitObserver||!Freeze)return EFI_UNSUPPORTED;
  if(mUsbHandoff.Smmu)return EFI_ALREADY_STARTED;
  mUsbHandoff.Smmu=Smmu;mUsbHandoff.Freeze=Freeze;
  S=gBS->CreateEvent(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Prepare,NULL,&mUsbHandoff.AcpiEvent);
  if(S!=EFI_SUCCESS)return Exact(S);
  S=gBS->RegisterProtocolNotify(&gEfiAcpiTableProtocolGuid,mUsbHandoff.AcpiEvent,&mUsbHandoff.Registration);
  if(S!=EFI_SUCCESS)return Exact(S);
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Prepare,NULL,&gEfiEndOfDxeEventGroupGuid,&mUsbHandoff.EndEvent);
  if(S!=EFI_SUCCESS)return Exact(S);
  S=gBS->CreateEventEx(EVT_NOTIFY_SIGNAL,TPL_CALLBACK,Prepare,NULL,&gEfiEventReadyToBootGuid,&mUsbHandoff.ReadyEvent);
  if(S!=EFI_SUCCESS)return Exact(S);
  S=Smmu->SetExitPrepareObserver(Smmu,&mUsbHandoff,BeforeRevoke,&mUsbHandoff);
  if(S!=EFI_SUCCESS)return Exact(S);mUsbHandoff.PrepareObserver=TRUE;
  S=Smmu->SetExitObserver(Smmu,&mUsbHandoff,AfterRevoke,&mUsbHandoff);
  if(S!=EFI_SUCCESS)return Exact(S);mUsbHandoff.CompleteObserver=TRUE;
  Prepare(NULL,NULL);return EFI_SUCCESS;
}
