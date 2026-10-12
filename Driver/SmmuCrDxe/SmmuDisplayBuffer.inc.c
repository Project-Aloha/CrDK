/* Typed frame/command mapping and retained cleanup. Included by SmmuCrDxe.
 * SPDX-License-Identifier: MIT */
STATIC BOOLEAN DisplayMappingProtected(CONST CR_SMMU_MAPPING *Map)
{ return Map&&Map->DisplayOwner!=NULL; }
STATIC BOOLEAN DisplayAllocationOverlap(UINT64 A,UINT64 Bytes)
{
  if(!Bytes||A>MAX_UINT64-Bytes)return TRUE;
  for(UINT32 I=0;I<2;++I){CR_SMMU_DISPLAY_BUFFER *B=&mDisplayBuffers[I];
    UINT64 N=(UINT64)B->Pages*SMMU_PAGE_SIZE;
    if(B->InUse&&B->Address&&(B->Address>MAX_UINT64-N||
       RangesOverlap(A,Bytes,B->Address,N)))return TRUE;}
  return FALSE;
}
STATIC BOOLEAN DisplayBackingConflictLocked(CR_SMMU_DISPLAY_BUFFER *Owner)
{
  UINT64 Bytes=(UINT64)Owner->Pages*SMMU_PAGE_SIZE;
  if(BufferMappingInUse(Owner->Address,Bytes))return TRUE;
  for(UINT32 I=0;I<2;++I){CR_SMMU_DISPLAY_BUFFER *B=&mDisplayBuffers[I];
    if(B!=Owner&&B->InUse&&B->Address&&RangesOverlap(Owner->Address,Bytes,
       B->Address,(UINT64)B->Pages*SMMU_PAGE_SIZE))return TRUE;}
  for(UINT32 I=0;I<CR_SMMU_MAX_DOMAINS;++I){CR_SMMU_DOMAIN *D=&mSmmu.Domains[I];
    if(D->Tables&&RangesOverlap(Owner->Address,Bytes,D->TableAddress,
       (UINT64)SMMU_TABLE_PAGES*SMMU_PAGE_SIZE))return TRUE;}
  for(UINTN I=0;I<ARRAY_SIZE(mSmmu.DmaBuffers);++I){CR_SMMU_DMA_BUFFER *B=&mSmmu.DmaBuffers[I];
    if((B->InUse||B->Busy)&&B->Address&&RangesOverlap(Owner->Address,Bytes,
       B->Address,(UINT64)B->Pages*SMMU_PAGE_SIZE))return TRUE;}
  return FALSE;
}
STATIC EFI_STATUS DisplayBufferArgumentsMode(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease,BOOLEAN ReadOnly)
{
  EFI_STATUS S=DisplayArgumentsMode(This,Group,ReadOnly);if(S!=EFI_SUCCESS)return S;
  if(!Lease||DisplayAlias(Lease,sizeof(*Lease),Group,sizeof(*Group))||
     DisplayAlias(Lease,sizeof(*Lease),&mSmmu,sizeof(mSmmu))||
     DisplayAlias(Lease,sizeof(*Lease),&mDisplayGroup,sizeof(mDisplayGroup))||
     DisplayAlias(Lease,sizeof(*Lease),mDisplayBuffers,sizeof(mDisplayBuffers)))return EFI_INVALID_PARAMETER;
  if(!DisplayGroupOwner(Group))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS DisplayBufferArguments(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{ return DisplayBufferArgumentsMode(This,Group,Lease,FALSE); }
STATIC CR_SMMU_DISPLAY_BUFFER *DisplayBufferFind(EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{
  for(UINT32 I=0;I<2;++I){CR_SMMU_DISPLAY_BUFFER *B=&mDisplayBuffers[I];
    if(B->InUse&&B->Client==Lease&&Lease->Token==B&&Lease->Generation==B->Generation&&
       B->GroupGeneration==mDisplayGroup.Generation)return B;}
  return NULL;
}
STATIC EFI_STATUS DisplayBufferDone(CR_SMMU_DISPLAY_BUFFER *B,EFI_STATUS Status)
{
  EfiAcquireLock(&mSmmu.Lock);B->LastStatus=DisplayExact(Status);
  if(B->LastStatus!=EFI_SUCCESS){B->CleanupOnly=TRUE;B->Ready=FALSE;}
  B->Busy=FALSE;EfiReleaseLock(&mSmmu.Lock);return DisplayExact(Status);
}
STATIC EFI_STATUS DisplayBufferDescriptor(CR_SMMU_DISPLAY_BUFFER *B,UINT64 Cache,
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR *Out)
{
  if(!gDS||!gDS->GetMemorySpaceDescriptor)return EFI_NOT_READY;
  EFI_STATUS S=DisplayExact(gDS->GetMemorySpaceDescriptor(B->Address,Out));
  UINT64 Bytes=(UINT64)B->Pages*SMMU_PAGE_SIZE;
  if(S!=EFI_SUCCESS)return S;
  if(!B->Address||!Bytes||B->Address>MAX_UINT64-Bytes||
     Out->BaseAddress>B->Address||Out->Length>MAX_UINT64-Out->BaseAddress||
     B->Address+Bytes>Out->BaseAddress+Out->Length||
     Out->GcdMemoryType!=(B->BorrowedFrame?EfiGcdMemoryTypeReserved:EfiGcdMemoryTypeSystemMemory)||
     (Out->Attributes&(EFI_MEMORY_RP|EFI_MEMORY_RO))||!(Out->Capabilities&EFI_MEMORY_WC)||
     (Cache&&(Out->Attributes&EFI_CACHE_ATTRIBUTE_MASK)!=Cache))return EFI_ACCESS_DENIED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS DisplayBufferCpuReady(CR_SMMU_DISPLAY_BUFFER *B)
{
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;EFI_STATUS S=DisplayBufferDescriptor(B,0,&D);
  if(S!=EFI_SUCCESS)return S;
  UINT64 Cache=D.Attributes&EFI_CACHE_ATTRIBUTE_MASK;
  if(Cache!=EFI_MEMORY_WB&&Cache!=EFI_MEMORY_WT&&Cache!=EFI_MEMORY_WC)return EFI_UNSUPPORTED;
  if(!gDS->SetMemorySpaceAttributes)return EFI_NOT_READY;
  B->OriginalAttributes=D.Attributes;B->AttributesTouched=TRUE;
  WriteBackInvalidateDataCacheRange((VOID *)(UINTN)B->Address,B->Pages*SMMU_PAGE_SIZE);
  S=DisplayExact(gDS->SetMemorySpaceAttributes(B->Address,B->Pages*SMMU_PAGE_SIZE,
    (D.Attributes&~EFI_CACHE_ATTRIBUTE_MASK)|EFI_MEMORY_WC));
  if(S==EFI_SUCCESS)S=DisplayBufferDescriptor(B,EFI_MEMORY_WC,&D);
  if(S==EFI_SUCCESS){EfiAcquireLock(&mSmmu.Lock);
    if(!DisplayCurrent()||!mDisplayGroup.Complete)S=EFI_NOT_READY;
    EfiReleaseLock(&mSmmu.Lock);}
  B->CpuReady=S==EFI_SUCCESS;return S;
}
STATIC EFI_STATUS DisplayBufferNew(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,UINT32 Kind,VOID *CpuAddress,
  EFI_PHYSICAL_ADDRESS Address,UINTN Bytes,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{
  EFI_STATUS S=DisplayBufferArguments(This,Group,Lease);if(S!=EFI_SUCCESS)return S;
  if(!Bytes||Bytes>MAX_UINTN-(SMMU_PAGE_SIZE-1U)||Bytes>SMMU_IOVA_SIZE-SMMU_PAGE_SIZE)return EFI_INVALID_PARAMETER;
  UINTN Pages=(Bytes+SMMU_PAGE_SIZE-1U)/SMMU_PAGE_SIZE;
  if(Kind==CrSmmuDisplayFrame&&(!CpuAddress||(UINT64)(UINTN)CpuAddress!=Address||
     (Address&(SMMU_PAGE_SIZE-1))||Address>MAX_UINT32||
     (UINT64)Pages*SMMU_PAGE_SIZE>0x100000000ULL-Address))return EFI_INVALID_PARAMETER;
  if(Kind==CrSmmuDisplayFrame&&(
     DisplayAlias(CpuAddress,Pages*SMMU_PAGE_SIZE,Group,sizeof(*Group))||
     DisplayAlias(CpuAddress,Pages*SMMU_PAGE_SIZE,Lease,sizeof(*Lease))||
     DisplayAlias(CpuAddress,Pages*SMMU_PAGE_SIZE,&mSmmu,sizeof(mSmmu))||
     DisplayAlias(CpuAddress,Pages*SMMU_PAGE_SIZE,&mDisplayGroup,sizeof(mDisplayGroup))||
     DisplayAlias(CpuAddress,Pages*SMMU_PAGE_SIZE,mDisplayBuffers,sizeof(mDisplayBuffers))))return EFI_INVALID_PARAMETER;
  EfiAcquireLock(&mSmmu.Lock);
  CR_SMMU_DISPLAY_BUFFER *B=&mDisplayBuffers[Kind];
  if(!DisplayCurrent()||!mDisplayGroup.Complete||mDisplayGroup.Busy)S=EFI_NOT_READY;
  else if(B->InUse||Lease->Token||Lease->Generation)S=EFI_ALREADY_STARTED;
  else if(mDisplayBufferGeneration==MAX_UINT64)S=EFI_OUT_OF_RESOURCES;
  else if(Kind==CrSmmuDisplayFrame&&BufferMappingInUse(Address,Pages*SMMU_PAGE_SIZE))S=EFI_ACCESS_DENIED;
  if(S==EFI_SUCCESS){
    ZeroMem(B,sizeof(*B));B->InUse=B->Busy=TRUE;B->Client=Lease;B->Kind=Kind;
    B->Generation=++mDisplayBufferGeneration;B->GroupGeneration=mDisplayGroup.Generation;
    B->Bytes=Bytes;B->Pages=Pages;B->BorrowedFrame=Kind==CrSmmuDisplayFrame;
    B->Address=Address;Lease->Token=B;Lease->Generation=B->Generation;
  }
  EfiReleaseLock(&mSmmu.Lock);if(S!=EFI_SUCCESS)return S;
  if(Kind==CrSmmuDisplayCommand){
    if(!gBS||!gBS->AllocatePages||!gBS->FreePages)return DisplayBufferDone(B,EFI_NOT_READY);
    EFI_PHYSICAL_ADDRESS Maximum=MAX_UINT32,Allocated=Maximum;
    EFI_STATUS Raw=gBS->AllocatePages(AllocateMaxAddress,EfiBootServicesData,Pages,&Allocated);
    S=DisplayExact(Raw);
    if(S!=EFI_SUCCESS){
      if(Allocated!=Maximum||!EFI_ERROR(Raw)){B->Address=Allocated;B->AllocationUncertain=TRUE;}
      return DisplayBufferDone(B,S);
    }
    B->Address=Allocated;B->Allocated=TRUE;
    if(!Allocated||(Allocated&(SMMU_PAGE_SIZE-1))||
       DisplayAlias((VOID *)(UINTN)Allocated,Pages*SMMU_PAGE_SIZE,Group,sizeof(*Group))||
       DisplayAlias((VOID *)(UINTN)Allocated,Pages*SMMU_PAGE_SIZE,Lease,sizeof(*Lease))||
       DisplayAlias((VOID *)(UINTN)Allocated,Pages*SMMU_PAGE_SIZE,&mSmmu,sizeof(mSmmu))||
       DisplayAlias((VOID *)(UINTN)Allocated,Pages*SMMU_PAGE_SIZE,&mDisplayGroup,sizeof(mDisplayGroup))||
       DisplayAlias((VOID *)(UINTN)Allocated,Pages*SMMU_PAGE_SIZE,mDisplayBuffers,sizeof(mDisplayBuffers))){
      B->AllocationUncertain=TRUE;return DisplayBufferDone(B,EFI_COMPROMISED_DATA);
    }
    if(!Allocated||(Allocated&(SMMU_PAGE_SIZE-1))||Allocated>MAX_UINT32||
       (UINT64)Pages*SMMU_PAGE_SIZE>0x100000000ULL-Allocated||!AddressRangeFitsDevice(Allocated,Pages))
      return DisplayBufferDone(B,EFI_OUT_OF_RESOURCES);
  }
  EfiAcquireLock(&mSmmu.Lock);BOOLEAN Conflict=DisplayBackingConflictLocked(B);
  EfiReleaseLock(&mSmmu.Lock);
  if(Conflict){if(B->Allocated)B->AllocationUncertain=TRUE;return DisplayBufferDone(B,EFI_ACCESS_DENIED);}
  return DisplayBufferDone(B,DisplayBufferCpuReady(B));
}
STATIC EFI_STATUS EFIAPI CrSmmuBindDisplayFrame(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,VOID *CpuAddress,EFI_PHYSICAL_ADDRESS Physical,
  UINTN Bytes,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{ return DisplayBufferNew(This,Group,CrSmmuDisplayFrame,CpuAddress,Physical,Bytes,Lease); }
STATIC EFI_STATUS EFIAPI CrSmmuAllocateDisplayCommand(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,UINTN Bytes,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{ return DisplayBufferNew(This,Group,CrSmmuDisplayCommand,NULL,0,Bytes,Lease); }
STATIC EFI_STATUS DisplayBufferBeginMode(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease,
  BOOLEAN Current,BOOLEAN ReadOnly,CR_SMMU_DISPLAY_BUFFER **Out)
{
  EFI_STATUS S=DisplayBufferArgumentsMode(This,Group,Lease,ReadOnly);if(S!=EFI_SUCCESS)return S;
  EfiAcquireLock(&mSmmu.Lock);CR_SMMU_DISPLAY_BUFFER *B=DisplayBufferFind(Lease);
  if(!B)S=EFI_INVALID_PARAMETER;
  else if(B->Busy||mDisplayGroup.Busy)S=EFI_NOT_READY;
  else if(Current&&(!DisplayCurrent()||!mDisplayGroup.Complete))S=EFI_NOT_READY;
  else{B->Busy=TRUE;*Out=B;}
  EfiReleaseLock(&mSmmu.Lock);return S;
}
STATIC EFI_STATUS DisplayBufferBegin(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease,
  BOOLEAN Current,CR_SMMU_DISPLAY_BUFFER **Out)
{ return DisplayBufferBeginMode(This,Group,Lease,Current,FALSE,Out); }
STATIC EFI_STATUS EFIAPI CrSmmuPrepareDisplayBuffer(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{
  CR_SMMU_DISPLAY_BUFFER *B;EFI_STATUS S=DisplayBufferBegin(This,Group,Lease,TRUE,&B);
  if(S!=EFI_SUCCESS)return S;
  if(B->CleanupOnly||!B->CpuReady||B->Mapping)return DisplayBufferDone(B,EFI_NOT_READY);
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=DisplayBufferDescriptor(B,EFI_MEMORY_WC,&D);
  if(S!=EFI_SUCCESS)return DisplayBufferDone(B,S);
  EfiAcquireLock(&mSmmu.Lock);UINTN FirstPage,Index;CR_SMMU_MAPPING *Map=NULL;
  S=DisplayRoutesLocked(FALSE);
  for(Index=0;Index<ARRAY_SIZE(mSmmu.Mappings);++Index)if(!mSmmu.Mappings[Index].InUse){Map=&mSmmu.Mappings[Index];break;}
  if(S==EFI_SUCCESS&&(!Map||!FindIovaRange(B->Pages,&FirstPage)))S=EFI_OUT_OF_RESOURCES;
  if(S==EFI_SUCCESS){
    ZeroMem(Map,sizeof(*Map));Map->Signature=CR_SMMU_MAPPING_SIGNATURE;Map->InUse=TRUE;
    Map->Operation=EdkiiIoMmuOperationBusMasterRead64;Map->HostAddress=Map->PhysicalBase=B->Address;
    Map->IovaBase=SMMU_IOVA_BASE+(UINT64)FirstPage*SMMU_PAGE_SIZE;Map->Pages=B->Pages;
    Map->NumberOfBytes=B->Bytes;Map->DisplayOwner=B;B->Mapping=Map;B->Iova=Map->IovaBase;
    /* The map journal exists before any leaf/TLB effect. Partial grant errors
     * retain its IOVA and CPU allocation even if no DomainAccess committed. */
    for(UINT32 I=0;I<4&&S==EFI_SUCCESS;++I){
      CR_SMMU_DOMAIN *Dom=FindDomainBySid(mDisplayGroup.TargetIdentity.Streams[I]);
      if(!Dom||!FindDomainIndex(Dom,&Index)){S=EFI_NOT_READY;break;}
      S=DisplayExact((EFI_STATUS)SmmuSetMapping(&Dom->Domain,B->Iova,B->Address,B->Pages,EDKII_IOMMU_ACCESS_READ));
      if(S==EFI_SUCCESS){Map->DomainAccess[Index]=EDKII_IOMMU_ACCESS_READ;B->GrantedMask|=1U<<I;}
    }
    if(S==EFI_SUCCESS)B->Ready=TRUE;
  }
  EfiReleaseLock(&mSmmu.Lock);return DisplayBufferDone(B,S);
}
STATIC BOOLEAN DisplayJournalReadyLocked(VOID)
{
  if(!DisplayCurrent()||mDisplayGroup.Busy||mDisplayGroup.NextAttach!=4||
     !mDisplayGroup.Core.Retired||mSmmu.Device.SyncFailed)return FALSE;
  for(UINT32 I=0;I<4;++I){
    CR_SMMU_DOMAIN *D=FindDomainBySid(mDisplayGroup.TargetIdentity.Streams[I]);
    if(!D||D->Poisoned||D->Domain.Failed||D->Segment||D->Bdf!=CR_SMMU_BDF_UNKNOWN||
       D->Domain.Device!=&mSmmu.Device)return FALSE;
  }
  return TRUE;
}
STATIC EFI_STATUS EFIAPI CrSmmuQueryDisplayBuffer(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease,
  EFI_SMMU_CR_DISPLAY_BUFFER_INFO *Out)
{
  EFI_STATUS S=DisplayBufferArgumentsMode(This,Group,Lease,TRUE);if(S!=EFI_SUCCESS)return S;
  if(!Out||DisplayAlias(Out,sizeof(*Out),Lease,sizeof(*Lease))||
     DisplayAlias(Out,sizeof(*Out),Group,sizeof(*Group))||DisplayAlias(Out,sizeof(*Out),&mSmmu,sizeof(mSmmu))||
     DisplayAlias(Out,sizeof(*Out),&mDisplayGroup,sizeof(mDisplayGroup))||
     DisplayAlias(Out,sizeof(*Out),mDisplayBuffers,sizeof(mDisplayBuffers)))return EFI_INVALID_PARAMETER;
  EFI_SMMU_CR_DISPLAY_BUFFER_INFO Info;ZeroMem(&Info,sizeof(Info));
  EfiAcquireLock(&mSmmu.Lock);CR_SMMU_DISPLAY_BUFFER *B=DisplayBufferFind(Lease);
  if(!B)S=EFI_INVALID_PARAMETER;else if(B->Busy)S=EFI_NOT_READY;
  else{
    Info.Size=sizeof(Info);Info.Revision=EFI_SMMU_CR_DISPLAY_BUFFER_INFO_REVISION;Info.Kind=B->Kind;Info.Generation=B->Generation;
    Info.OwnerGeneration=mDisplayGroup.OwnerGeneration;Info.HostAddress=B->Address;Info.DeviceAddress=B->Iova;
    Info.Bytes=B->Bytes;Info.PageBytes=B->Pages*SMMU_PAGE_SIZE;Info.GrantedMask=B->GrantedMask;Info.LastStatus=B->LastStatus;
    Info.Flags=(B->Allocated?CR_SMMU_DISPLAY_ALLOCATED:0)|(B->BorrowedFrame?CR_SMMU_DISPLAY_BORROWED_FRAME:0)|
      (B->CpuReady?CR_SMMU_DISPLAY_CPU_READY:0)|(B->Mapping?CR_SMMU_DISPLAY_MAPPING:0)|
      (B->Ready?CR_SMMU_DISPLAY_READY:0)|(B->CleanupOnly?CR_SMMU_DISPLAY_CLEANUP_ONLY:0)|
      (B->AllocationUncertain?CR_SMMU_DISPLAY_ALLOCATION_UNCERTAIN:0)|
      (DisplayJournalReadyLocked()?CR_SMMU_DISPLAY_OWNER_READY:0);
  }
  EfiReleaseLock(&mSmmu.Lock);if(S==EFI_SUCCESS)*Out=Info;return S;
}
STATIC EFI_STATUS DisplayBufferCheckLocked(CR_SMMU_DISPLAY_BUFFER *B)
{
  if(!DisplayCurrent()||!B->Ready||B->CleanupOnly||!B->CpuReady||B->GrantedMask!=15||
     !B->Mapping||FindMapping(B->Mapping)!=B->Mapping||B->Mapping->DisplayOwner!=B||
     B->Mapping->PhysicalBase!=B->Address||B->Mapping->HostAddress!=B->Address||
     B->Mapping->Pages!=B->Pages||B->Mapping->IovaBase!=B->Iova||
     B->Mapping->BounceAddress||B->Mapping->DeviceWriteGranted)return EFI_NOT_READY;
  EFI_STATUS S=DisplayRoutesLocked(FALSE);UINT32 Seen=0;
  for(UINT32 I=0;I<CR_SMMU_MAX_DOMAINS&&S==EFI_SUCCESS;++I){
    CR_SMMU_DOMAIN *D=&mSmmu.Domains[I];UINT32 Bit=0;
    if(D->InUse)for(UINT32 K=0;K<4;++K)if(D->Sid==mDisplayGroup.TargetIdentity.Streams[K])Bit=1U<<K;
    if(B->Mapping->DomainAccess[I]!=(Bit?EDKII_IOMMU_ACCESS_READ:0))return EFI_NOT_READY;
    if(Bit){if(Seen&Bit)return EFI_NOT_READY;Seen|=Bit;SmmuMappingEvidence E;
      S=DisplayExact((EFI_STATUS)SmmuQueryMappingEvidence(&D->Domain,B->Iova,B->Address,B->Pages,EDKII_IOMMU_ACCESS_READ,&E));}
  }
  return S==EFI_SUCCESS&&Seen!=15?EFI_NOT_READY:S;
}
STATIC EFI_STATUS EFIAPI CrSmmuCheckDisplayBuffer(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{
  CR_SMMU_DISPLAY_BUFFER *B;EFI_STATUS S=DisplayBufferBeginMode(This,Group,Lease,TRUE,TRUE,&B);if(S!=EFI_SUCCESS)return S;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=DisplayBufferDescriptor(B,EFI_MEMORY_WC,&D);
  EfiAcquireLock(&mSmmu.Lock);if(S==EFI_SUCCESS)S=DisplayBufferCheckLocked(B);
  B->LastStatus=S;B->Busy=FALSE;EfiReleaseLock(&mSmmu.Lock);return S;
}
STATIC EFI_STATUS EFIAPI CrSmmuPreserveDisplayFrame(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{
  CR_SMMU_DISPLAY_BUFFER *B;EFI_STATUS S=DisplayBufferBegin(This,Group,Lease,TRUE,&B);if(S!=EFI_SUCCESS)return S;
  EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=DisplayBufferDescriptor(B,EFI_MEMORY_WC,&D);
  EfiAcquireLock(&mSmmu.Lock);
  if(S==EFI_SUCCESS&&!B->BorrowedFrame)S=EFI_UNSUPPORTED;
  if(S==EFI_SUCCESS)S=DisplayBufferCheckLocked(B);
  if(S==EFI_SUCCESS)B->Mapping->PreserveAtExit=TRUE;
  B->LastStatus=S;B->Busy=FALSE;EfiReleaseLock(&mSmmu.Lock);return S;
}
STATIC EFI_STATUS EFIAPI CrSmmuReleaseDisplayBuffer(EFI_SMMU_CR_PROTOCOL *This,
  EFI_SMMU_CR_DISPLAY_GROUP *Group,EFI_SMMU_CR_DISPLAY_LEASE *Lease)
{
  CR_SMMU_DISPLAY_BUFFER *B;EFI_STATUS S=DisplayBufferBegin(This,Group,Lease,FALSE,&B);if(S!=EFI_SUCCESS)return S;
  B->CleanupOnly=TRUE;B->Ready=FALSE;
  S=DisplayExact(mDisplayGroup.VerifyStopped(mDisplayGroup.Context,mDisplayGroup.OwnerGeneration));
  if(S!=EFI_SUCCESS)return DisplayBufferDone(B,S);
  if(B->AllocationUncertain)return DisplayBufferDone(B,EFI_NOT_READY);
  if(B->Mapping){
    EfiAcquireLock(&mSmmu.Lock);B->Mapping->PreserveAtExit=FALSE;
    for(UINT32 I=0;I<4&&S==EFI_SUCCESS;++I){
      CR_SMMU_DOMAIN *D=FindDomainBySid(mDisplayGroup.TargetIdentity.Streams[I]);UINTN Index;
      if(!D||!FindDomainIndex(D,&Index)){S=EFI_NOT_READY;break;}
      S=DisplayExact((EFI_STATUS)SmmuSetMapping(&D->Domain,B->Iova,0,B->Pages,0));
      if(S==EFI_SUCCESS){SmmuMappingEvidence E;S=DisplayExact((EFI_STATUS)SmmuQueryRevokedMapping(&D->Domain,B->Iova,B->Pages,&E));}
      if(S==EFI_SUCCESS){B->Mapping->DomainAccess[Index]=0;B->GrantedMask&=~(1U<<I);}
    }
    if(S==EFI_SUCCESS){ReleaseIovaRange(B->Iova,B->Pages);ZeroMem(B->Mapping,sizeof(*B->Mapping));B->Mapping=NULL;B->Iova=0;}
    EfiReleaseLock(&mSmmu.Lock);if(S!=EFI_SUCCESS)return DisplayBufferDone(B,S);
  }
  if(B->AttributesTouched){
    EFI_GCD_MEMORY_SPACE_DESCRIPTOR D;S=DisplayBufferDescriptor(B,0,&D);
    if(S!=EFI_SUCCESS)return DisplayBufferDone(B,S);
    WriteBackInvalidateDataCacheRange((VOID *)(UINTN)B->Address,B->Pages*SMMU_PAGE_SIZE);
    S=DisplayExact(gDS->SetMemorySpaceAttributes(B->Address,B->Pages*SMMU_PAGE_SIZE,B->OriginalAttributes));
    if(S==EFI_SUCCESS)S=DisplayBufferDescriptor(B,B->OriginalAttributes&EFI_CACHE_ATTRIBUTE_MASK,&D);
    if(S!=EFI_SUCCESS||D.Attributes!=B->OriginalAttributes)return DisplayBufferDone(B,S==EFI_SUCCESS?EFI_NOT_READY:S);
    B->AttributesTouched=FALSE;
  }
  if(B->Allocated){
    if(!gBS||!gBS->FreePages)return DisplayBufferDone(B,EFI_NOT_READY);
    EFI_STATUS Raw=gBS->FreePages(B->Address,B->Pages);S=DisplayExact(Raw);
    if(S!=EFI_SUCCESS){if(!EFI_ERROR(Raw))B->AllocationUncertain=TRUE;return DisplayBufferDone(B,S);}
    B->Allocated=FALSE;
  }
  EfiAcquireLock(&mSmmu.Lock);ZeroMem(Lease,sizeof(*Lease));ZeroMem(B,sizeof(*B));EfiReleaseLock(&mSmmu.Lock);
  return EFI_SUCCESS;
}
