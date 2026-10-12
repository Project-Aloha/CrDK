/* Immutable display handoff storage and producer rendezvous. MIT */
#include <PiDxe.h>
#include <Library/MdssHandoffUefiLib.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>

/* Every synchronous provider operation requires exact completion. A warning
   cannot attest to reservation, visibility, or successful retirement. */
STATIC EFI_STATUS Incomplete(EFI_STATUS Status) {
  return EFI_ERROR(Status)?Status:EFI_NOT_READY;
}
STATIC BOOLEAN PrepareOpen(CONST CR_MDSS_UEFI_HANDOFF *S) {
  return !S->Freezing&&!S->Frozen&&!S->SmmuComplete&&!S->Published;
}
STATIC UINT32 Registered(CONST CR_MDSS_UEFI_HANDOFF *S) {
  UINT32 Owners=CR_MDSS_NATIVE_SMMU;
  for(UINTN I=0;I<ARRAY_SIZE(S->Producers);++I)Owners|=S->Producers[I].Owner;
  return Owners;
}
EFI_STATUS CrMdssUefiSetParticipant(CR_MDSS_UEFI_HANDOFF *S,UINT32 Owner,VOID *Context,
    EFI_MDSS_HANDOFF_FREEZE Freeze,EFI_MDSS_HANDOFF_SNAPSHOT Snapshot) {
  UINTN Free=ARRAY_SIZE(S->Producers);
  if(!S||!Context||!Owner||(Owner&(Owner-1))||(Owner&~CR_MDSS_NATIVE_ALL_OWNERS)||
     Owner==CR_MDSS_NATIVE_SMMU||((Freeze==NULL)!=(Snapshot==NULL)))return EFI_INVALID_PARAMETER;
  if(S->Freezing||S->Frozen||S->Published||S->SmmuComplete)return EFI_ACCESS_DENIED;
  for(UINTN I=0;I<ARRAY_SIZE(S->Producers);++I) {
    if(S->Producers[I].Owner==Owner) {
      if(S->Producers[I].Context!=Context)return EFI_ALREADY_STARTED;
      if(!Freeze){ZeroMem(&S->Producers[I],sizeof(S->Producers[I]));return EFI_SUCCESS;}
      return S->Producers[I].Freeze==Freeze&&S->Producers[I].Snapshot==Snapshot?EFI_SUCCESS:EFI_ALREADY_STARTED;
    }
    if(!S->Producers[I].Owner&&Free==ARRAY_SIZE(S->Producers))Free=I;
  }
  if(!Freeze)return EFI_NOT_FOUND;
  if(Free==ARRAY_SIZE(S->Producers))return EFI_OUT_OF_RESOURCES;
  S->Producers[Free].Owner=Owner;S->Producers[Free].Context=Context;
  S->Producers[Free].Freeze=Freeze;S->Producers[Free].Snapshot=Snapshot;
  return EFI_SUCCESS;
}
STATIC BOOLEAN Covers(UINT64 B,UINT64 N,UINT64 P,UINT64 L) {
  return N&&L&&B<=MAX_UINT64-N&&P>=B&&L<=N&&P-B<=N-L;
}
/* The MMU and OS retain complete pages, including the unused pixel tail. */
STATIC BOOLEAN PageExtent(UINT64 Address,UINT64 Bytes,UINT64 *Base,UINT64 *Length) {
  UINT64 Offset=Address&EFI_PAGE_MASK;
  if(!Bytes||Bytes>MAX_UINT64-Offset)return FALSE;
  UINT64 Total=Bytes+Offset;
  if(Total>MAX_UINT64-EFI_PAGE_MASK)return FALSE;
  *Base=Address&~(UINT64)EFI_PAGE_MASK;
  *Length=(Total+EFI_PAGE_MASK)&~(UINT64)EFI_PAGE_MASK;
  return *Base<=MAX_UINT64-*Length;
}
STATIC EFI_STATUS NormalRam(UINT64 Address,UINT64 Bytes) {
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;
  EFI_STATUS R=gDS->GetMemorySpaceDescriptor(Address,&D);
  if(R!=EFI_SUCCESS)return Incomplete(R);
  if(!Covers(D.BaseAddress,D.Length,Address,Bytes)||
     (D.GcdMemoryType!=EfiGcdMemoryTypeSystemMemory&&D.GcdMemoryType!=EfiGcdMemoryTypeReserved)||
     (D.Attributes&(EFI_MEMORY_RP|EFI_MEMORY_WP|EFI_MEMORY_RO|EFI_MEMORY_UC|EFI_MEMORY_UCE))||
     !(D.Attributes&(EFI_MEMORY_WB|EFI_MEMORY_WT|EFI_MEMORY_WC)))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS NextCount(UINT64 *Count) {
  EFI_STATUS R;
  for(UINTN I=0;I<2&&!*Count;++I){R=gBS->GetNextMonotonicCount(Count);if(R!=EFI_SUCCESS){*Count=0;return Incomplete(R);}}
  return *Count?EFI_SUCCESS:EFI_NOT_READY;
}
EFI_STATUS CrMdssUefiPrepare(CR_MDSS_UEFI_HANDOFF *S) {
  EFI_STATUS R;UINTN Size,Key,Stride;UINT32 Version;
  CR_MDSS_NATIVE_LOCATOR L={0};
  if(!S)return EFI_INVALID_PARAMETER;
  if(S->Installed)return EFI_SUCCESS;
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  if(!S->Acpi){
    EFI_ACPI_TABLE_PROTOCOL *Acpi=NULL;
    R=gBS->LocateProtocol(&gEfiAcpiTableProtocolGuid,NULL,(VOID**)&Acpi);
    if(R!=EFI_SUCCESS)return Incomplete(R);
    S->Acpi=Acpi;
  }
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  if(!S->Acpi||!S->Acpi->InstallAcpiTable||!S->Acpi->UninstallAcpiTable)return EFI_UNSUPPORTED;
  R=NextCount(&S->Generation);if(R!=EFI_SUCCESS)return Incomplete(R);
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  R=NextCount(&S->TransferId);if(R!=EFI_SUCCESS)return Incomplete(R);
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  if(!S->RecordOwned){
    R=gBS->AllocatePages(AllocateAnyPages,EfiReservedMemoryType,EFI_SIZE_TO_PAGES(CR_MDSS_HANDOFF_RESERVATION_BYTES),&S->Record);
    if(R!=EFI_SUCCESS){S->Record=0;return Incomplete(R);}S->RecordOwned=TRUE;
  }
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  if(S->Record<0x100000||(S->Record&EFI_PAGE_MASK))return EFI_UNSUPPORTED;
  R=NormalRam(S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES);if(R!=EFI_SUCCESS)return Incomplete(R);
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  ZeroMem((VOID*)(UINTN)S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES);
  WriteBackDataCacheRange((VOID*)(UINTN)S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES);MemoryFence();
  if(!S->MemoryMap){
    Size=Stride=0;R=gBS->GetMemoryMap(&Size,NULL,&Key,&Stride,&Version);
    if(R!=EFI_BUFFER_TOO_SMALL)return EFI_ERROR(R)?R:EFI_COMPROMISED_DATA;
    if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
    if(Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>1024||Size>1024*1024-128*Stride)return EFI_OUT_OF_RESOURCES;
    Size+=128*Stride;if(Size<65536)Size=65536;
    R=gBS->AllocatePool(EfiBootServicesData,Size,(VOID**)&S->MemoryMap);
    if(R!=EFI_SUCCESS){S->MemoryMap=NULL;return Incomplete(R);}S->MapCapacity=Size;
  }
  if(!PrepareOpen(S))return EFI_ACCESS_DENIED;
  L.Signature=CR_MDSS_NATIVE_LOCATOR_SIGNATURE;L.Length=sizeof(L);L.Revision=1;
  CopyMem(L.OemId,"ALOHA ",6);CopyMem(L.OemTableId,"MDSHDK84",8);
  L.OemRevision=1;L.CreatorId=SIGNATURE_32('C','R','D','K');L.CreatorRevision=1;
  L.WireRevision=CR_MDSS_NATIVE_REVISION;L.RecordPhysical=S->Record;
  L.RecordLength=sizeof(CR_MDSS_NATIVE_HANDOFF);L.RecordGeneration=S->Generation;
  L.Checksum=CalculateCheckSum8((UINT8*)&L,sizeof(L));
  R=S->Acpi->InstallAcpiTable(S->Acpi,&L,sizeof(L),&S->TableKey);
  if(R!=EFI_SUCCESS)return Incomplete(R);S->Installed=TRUE;
  return PrepareOpen(S)?EFI_SUCCESS:EFI_ACCESS_DENIED;
}
EFI_STATUS CrMdssUefiRelease(CR_MDSS_UEFI_HANDOFF *S) {
  EFI_STATUS R;
  if(!S)return EFI_INVALID_PARAMETER;
  if(S->Freezing||S->Frozen||S->SmmuComplete||S->Published)return EFI_ACCESS_DENIED;
  if(S->Installed){R=S->Acpi->UninstallAcpiTable(S->Acpi,S->TableKey);if(R!=EFI_SUCCESS)return Incomplete(R);S->Installed=FALSE;S->TableKey=0;}
  if(S->MemoryMap){R=gBS->FreePool(S->MemoryMap);if(R!=EFI_SUCCESS)return Incomplete(R);S->MemoryMap=NULL;S->MapCapacity=0;}
  if(S->RecordOwned){R=gBS->FreePages(S->Record,EFI_SIZE_TO_PAGES(CR_MDSS_HANDOFF_RESERVATION_BYTES));if(R!=EFI_SUCCESS)return Incomplete(R);S->RecordOwned=FALSE;S->Record=0;}
  S->Generation=S->TransferId=0;S->Acpi=NULL;return EFI_SUCCESS;
}
EFI_STATUS CrMdssUefiFreeze(CR_MDSS_UEFI_HANDOFF *S) {
  EFI_STATUS R;
  if(!S)return EFI_INVALID_PARAMETER;
  if(S->Frozen)return S->FreezeStatus;
  if(S->Freezing)return EFI_NOT_READY;
  S->Freezing=TRUE;S->FreezeStatus=EFI_SUCCESS;
  /* DP stops transient scanout first, primary GOP closes software access,
     then the bridge and shared PHY close all mutation paths. */
  CONST UINT32 Order[]={CR_MDSS_NATIVE_DP,CR_MDSS_NATIVE_DSI,CR_MDSS_NATIVE_BRIDGE,CR_MDSS_NATIVE_PHY};
  for(UINTN J=0;J<ARRAY_SIZE(Order);++J)for(UINTN I=0;I<ARRAY_SIZE(S->Producers);++I) {
    if(S->Producers[I].Owner!=Order[J])continue;
    R=S->Producers[I].Freeze(S->Producers[I].Context);
    if(R==EFI_SUCCESS)S->Producers[I].Frozen=TRUE;
    else if(S->FreezeStatus==EFI_SUCCESS)S->FreezeStatus=EFI_ERROR(R)?R:EFI_NOT_READY;
  }
  if(Registered(S)!=CR_MDSS_NATIVE_ALL_OWNERS&&S->FreezeStatus==EFI_SUCCESS)S->FreezeStatus=EFI_NOT_FOUND;
  S->Frozen=TRUE;S->Freezing=FALSE;return S->FreezeStatus;
}
STATIC EFI_STATUS ReservedRange(CR_MDSS_UEFI_HANDOFF *S,UINTN Size,UINTN Stride,UINT64 Address,UINT64 Bytes) {
  BOOLEAN Found=FALSE;
  if(!Bytes||Address>MAX_UINT64-Bytes)return EFI_INVALID_PARAMETER;
  for(UINTN I=0;I<Size;I+=Stride){
    EFI_MEMORY_DESCRIPTOR D;CopyMem(&D,(UINT8*)S->MemoryMap+I,sizeof(D));
    if(!D.NumberOfPages||D.NumberOfPages>MAX_UINT64/EFI_PAGE_SIZE||(D.PhysicalStart&EFI_PAGE_MASK))return EFI_COMPROMISED_DATA;
    UINT64 Length=D.NumberOfPages*EFI_PAGE_SIZE;
    if(D.PhysicalStart>MAX_UINT64-Length)return EFI_COMPROMISED_DATA;
    if(D.PhysicalStart<Address+Bytes&&Address<D.PhysicalStart+Length){
      /* CoreGetMemoryMap includes GCD cache capabilities in Attribute.
         Qualify the actual CPU mapping separately through GCD below. */
      if(Found||!Covers(D.PhysicalStart,Length,Address,Bytes)||D.Type!=EfiReservedMemoryType)return EFI_ACCESS_DENIED;
      Found=TRUE;
    }
  }
  return Found?NormalRam(Address,Bytes):EFI_NOT_FOUND;
}
STATIC BOOLEAN Overlaps(UINT64 A,UINT64 N,UINT64 B,UINT64 M) {
  return N&&M&&A<=MAX_UINT64-N&&B<=MAX_UINT64-M&&A<B+M&&B<A+N;
}
STATIC EFI_STATUS DisjointRecord(CR_MDSS_UEFI_HANDOFF *S) {
  /* Shared native SMMU tables may appear in both route observations. They
     must never overlap the publication buffer or either scanout buffer. */
  for(UINTN I=0;I<CR_MDSS_NATIVE_ROUTES;++I){
    CONST CR_MDSS_NATIVE_ROUTE *P=&S->Local.Routes[I];
    UINT64 Base=0,Bytes=0;
    if(P->FrameBufferBytes&&!PageExtent(P->FrameBuffer,P->FrameBufferBytes,&Base,&Bytes))return EFI_COMPROMISED_DATA;
    if(Overlaps(S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES,Base,Bytes))return EFI_ACCESS_DENIED;
    for(UINTN J=0;J<P->StreamCount;++J){
      CONST SmmuMappingEvidence *E=&P->Streams[J];
      if(Overlaps(S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES,E->Tables,E->TableBytes))return EFI_ACCESS_DENIED;
      for(UINTN K=0;K<CR_MDSS_NATIVE_ROUTES;++K){
        CONST CR_MDSS_NATIVE_ROUTE *Q=&S->Local.Routes[K];
        Base=Bytes=0;
        if(Q->FrameBufferBytes&&!PageExtent(Q->FrameBuffer,Q->FrameBufferBytes,&Base,&Bytes))return EFI_COMPROMISED_DATA;
        if(Overlaps(E->Tables,E->TableBytes,Base,Bytes))return EFI_ACCESS_DENIED;
      }
    }
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS FinalMemory(CR_MDSS_UEFI_HANDOFF *S) {
  UINTN Size=S->MapCapacity,Key,Stride;UINT32 Version;
  EFI_STATUS R=gBS->GetMemoryMap(&Size,S->MemoryMap,&Key,&Stride,&Version);
  if(R!=EFI_SUCCESS)return Incomplete(R);
  if(Version!=EFI_MEMORY_DESCRIPTOR_VERSION||Stride<sizeof(EFI_MEMORY_DESCRIPTOR)||Stride>1024||
     (Stride&7)||Size>S->MapCapacity||Size%Stride)return EFI_COMPROMISED_DATA;
  R=ReservedRange(S,Size,Stride,S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES);if(EFI_ERROR(R))return R;
  for(UINTN I=0;I<CR_MDSS_NATIVE_ROUTES;++I){
    CR_MDSS_NATIVE_ROUTE *P=&S->Local.Routes[I];
    if(P->FrameBufferBytes){
      UINT64 Base,Bytes;
      if(!PageExtent(P->FrameBuffer,P->FrameBufferBytes,&Base,&Bytes))return EFI_COMPROMISED_DATA;
      R=ReservedRange(S,Size,Stride,Base,Bytes);if(EFI_ERROR(R))return R;
    }
    for(UINTN J=0;J<P->StreamCount;++J){
      R=ReservedRange(S,Size,Stride,P->Streams[J].Tables,P->Streams[J].TableBytes);if(EFI_ERROR(R))return R;
    }
  }
  return EFI_SUCCESS;
}
EFI_STATUS CrMdssUefiPublish(CR_MDSS_UEFI_HANDOFF *S,EFI_SMMU_CR_PROTOCOL *Smmu) {
  EFI_STATUS R;
  if(!S||!Smmu)return EFI_INVALID_PARAMETER;
  if(S->Published)return EFI_SUCCESS;
  if(!S->Installed||!S->RecordOwned||!S->MemoryMap||!S->Frozen||S->FreezeStatus!=EFI_SUCCESS||!S->SmmuComplete||
     Smmu->Revision<EFI_SMMU_CR_EXIT_REVOCATION_REVISION||!Smmu->QueryExitMapping||!Smmu->QueryExitRevokedMapping)return EFI_NOT_READY;
  ZeroMem(&S->Local,sizeof(S->Local));ZeroMem(S->Mappings,sizeof(S->Mappings));
  R=(EFI_STATUS)CrMdssHandoffInit(&S->Local,S->Generation,S->TransferId,S->Record,CR_MDSS_HANDOFF_RESERVATION_BYTES,
                                  CR_MDSS_NATIVE_ALL_OWNERS,Registered(S));if(EFI_ERROR(R))return R;
  for(UINTN I=0;I<ARRAY_SIZE(S->Producers);++I){
    if(!S->Producers[I].Frozen)return EFI_NOT_READY;
    VOID *Map=NULL;ZeroMem(&S->Participant,sizeof(S->Participant));S->Participant.Owner=S->Producers[I].Owner;
    R=S->Producers[I].Snapshot(S->Producers[I].Context,&S->Participant,&Map);if(R!=EFI_SUCCESS)return EFI_ERROR(R)?R:EFI_NOT_READY;
    if(S->Participant.Owner!=S->Producers[I].Owner)return EFI_COMPROMISED_DATA;
    if(S->Participant.Owner==CR_MDSS_NATIVE_DSI)S->Mappings[0]=Map;
    else if(S->Participant.Owner==CR_MDSS_NATIVE_DP)S->Mappings[1]=Map;
    else if(Map!=NULL)return EFI_COMPROMISED_DATA;
    R=(EFI_STATUS)CrMdssHandoffAppend(&S->Local,&S->Participant);if(EFI_ERROR(R))return R;
  }
  ZeroMem(&S->Participant,sizeof(S->Participant));S->Participant.Owner=CR_MDSS_NATIVE_SMMU;
  for(UINTN I=0;I<CR_MDSS_NATIVE_ROUTES;++I){
    CR_MDSS_NATIVE_ROUTE *P=&S->Local.Routes[I];CR_MDSS_NATIVE_MAPPING *M=&S->Participant.Mappings[I];
    M->Owner=I==0?CR_MDSS_NATIVE_DSI:CR_MDSS_NATIVE_DP;
    M->State=CR_MDSS_NATIVE_MAPPING_UNKNOWN;
    if(S->Mappings[I]==NULL){
      if(!P->DeviceAddress&&!P->MappingId)M->State=CR_MDSS_NATIVE_MAPPING_NONE;
      else M->Status=EFI_NOT_FOUND;
      continue;
    }
    R=P->State==CR_MDSS_NATIVE_LIVE?Smmu->QueryExitMapping(Smmu,S->Mappings[I],&S->Evidence):
                                  Smmu->QueryExitRevokedMapping(Smmu,S->Mappings[I],&S->Evidence);
    if(R!=EFI_SUCCESS){M->Status=EFI_ERROR(R)?R:EFI_NOT_READY;continue;}
    EFI_SMMU_CR_MAPPING_EVIDENCE *E=&S->Evidence;
    UINT64 Base,Bytes;
    if(E->Size!=sizeof(*E)||E->Revision!=1||!E->StreamCount||E->StreamCount>CR_MDSS_NATIVE_STREAMS||
       E->HostAddress!=P->FrameBuffer||E->NumberOfBytes!=P->FrameBufferBytes||E->DeviceAddress!=P->DeviceAddress||
       !PageExtent(P->FrameBuffer,P->FrameBufferBytes,&Base,&Bytes)||E->PhysicalBase!=Base||
       E->IovaBase!=P->DeviceAddress||(E->IovaBase&EFI_PAGE_MASK)||E->PageBytes!=Bytes||E->IovaBase>MAX_UINT64-Bytes){
      M->Status=EFI_COMPROMISED_DATA;continue;
    }
    M->MappingId=E->MappingId;M->StreamCount=E->StreamCount;
    M->State=P->State==CR_MDSS_NATIVE_LIVE?CR_MDSS_NATIVE_MAPPING_PRESERVED:CR_MDSS_NATIVE_MAPPING_REVOKED;
    CopyMem(M->Streams,E->Streams,E->StreamCount*sizeof(E->Streams[0]));
  }
  R=(EFI_STATUS)CrMdssHandoffAppend(&S->Local,&S->Participant);if(EFI_ERROR(R))return R;
  R=DisjointRecord(S);if(R!=EFI_SUCCESS)return R;
  R=FinalMemory(S);if(R!=EFI_SUCCESS)return Incomplete(R);
  R=(EFI_STATUS)CrMdssHandoffFinalize(&S->Local,2);if(EFI_ERROR(R))return R;
  volatile CR_MDSS_NATIVE_HANDOFF *W=(VOID*)(UINTN)S->Record;
  W->Sequence=1;MemoryFence();S->Local.Sequence=1;
  CopyMem((VOID*)W,&S->Local,sizeof(S->Local));WriteBackDataCacheRange((VOID*)W,sizeof(S->Local));MemoryFence();
  W->Sequence=2;WriteBackDataCacheRange((VOID*)&W->Sequence,sizeof(W->Sequence));MemoryFence();
  S->Local.Sequence=2;S->Published=TRUE;return EFI_SUCCESS;
}
