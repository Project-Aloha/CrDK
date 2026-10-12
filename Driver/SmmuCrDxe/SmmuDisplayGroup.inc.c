/* Shared inherited display group transaction. Included by SmmuCrDxe.
 * SPDX-License-Identifier: GPL-2.0-only */
STATIC EFI_STATUS DisplayExact(EFI_STATUS Status)
{ return Status==EFI_SUCCESS||EFI_ERROR(Status)?Status:EFI_NOT_READY; }
STATIC BOOLEAN DisplaySid(UINT16 Sid)
{
  CONST CR_MDSS_TARGET *T=mDisplayGroup.Active?&mDisplayGroup.TargetIdentity:CrDalGetMdssContext();
  if(!T||!T->RequiresStoppedDisplay)return FALSE;
  for(UINT32 I=0;I<4;++I)if(T->Streams[I]==Sid)return TRUE;
  return FALSE;
}
STATIC BOOLEAN DisplayTargetValid(CONST CR_MDSS_TARGET *T,UINT32 Soc)
{
  STATIC CONST UINT16 Sids[4]={0x820,0x822,0xc20,0xc22};
  if(!T||T->Revision!=CR_MDSS_TARGET_REVISION||T->Size!=sizeof(*T)||
     (Soc!=0x8250&&Soc!=0x8350)||T->Soc!=Soc||!T->RequiresStoppedDisplay||
     T->SmmuBase!=mSmmu.Device.Base||mSmmu.Device.Base!=0x15000000ULL||
     !mSmmu.Target||mSmmu.Target->Base!=T->SmmuBase||mSmmu.Target->Size!=0x100000)
    return FALSE;
  for(UINT32 I=0;I<4;++I)if(T->Streams[I]!=Sids[I])return FALSE;
  return TRUE;
}
STATIC BOOLEAN DisplaySame(CONST VOID *A,CONST VOID *B,UINTN Bytes)
{ CONST UINT8 *X=A,*Y=B;while(Bytes--)if(*X++!=*Y++)return FALSE;return TRUE; }
STATIC BOOLEAN DisplayCurrent(VOID)
{
  CONST CR_MDSS_TARGET *T=CrDalGetMdssContext();
  return mDisplayGroup.Active&&!mDisplayGroup.Failed&&mDisplayGroup.Target==T&&
    mDisplayGroup.SmmuTarget==mSmmu.Target&&DisplayTargetValid(T,mDisplayGroup.TargetIdentity.Soc)&&
    DisplaySame(T,&mDisplayGroup.TargetIdentity,sizeof(*T))&&
    mDisplayGroup.Client&&mDisplayGroup.Client->Token==&mDisplayGroup&&
    mDisplayGroup.Client->Generation==mDisplayGroup.Generation;
}
STATIC BOOLEAN DisplayGroupOwner(EFI_SMMU_CR_DISPLAY_GROUP *Group)
{
  return Group&&mDisplayGroup.Active&&Group==mDisplayGroup.Client&&
    Group->Token==&mDisplayGroup&&Group->Generation==mDisplayGroup.Generation;
}
STATIC CR_STATUS DisplayCoreReady(VOID *Context,UINT64 Generation)
{
  if(Context!=&mDisplayGroup||!mSmmu.Initialized||mSmmu.ExistingIoMmu||
     (mSmmu.AfterExitBootServices&&!(mDisplayGroup.RetirementCheck&&mSmmu.ExitPreparing))||
     mSmmu.Lock.Lock!=EfiLockAcquired||
     (!mDisplayGroup.Busy&&!mDisplayGroup.RetirementCheck)||
     !DisplayCurrent()||Generation!=mDisplayGroup.Generation)
    return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS DisplayCoreRead(VOID *Context,UINT32 Offset,UINT32 *Value)
{ (VOID)Context;*Value=mSmmu.Device.Io.Read32(mSmmu.Device.Io.Cookie,mSmmu.Device.Base+Offset);return CR_SUCCESS; }
STATIC CR_STATUS DisplayCoreWrite(VOID *Context,UINT32 Offset,UINT32 Value)
{
  (VOID)Context;if(mDisplayGroup.RetirementCheck||!mDisplayGroup.Busy||mSmmu.AfterExitBootServices)
    return EFI_ACCESS_DENIED;
  mSmmu.Device.Io.Write32(mSmmu.Device.Io.Cookie,mSmmu.Device.Base+Offset,Value);return CR_SUCCESS;
}
STATIC CR_STATUS DisplayCoreFence(VOID *Context)
{ (VOID)Context;MemoryFence();return CR_SUCCESS; }
STATIC CR_STATUS DisplayCoreDelay(VOID *Context,UINT32 Us)
{ (VOID)Context;mSmmu.Device.Io.DelayUs(mSmmu.Device.Io.Cookie,Us);return CR_SUCCESS; }
STATIC CONST CR_SMMU_DISPLAY_GROUP_CORE_IO mDisplayCoreIo={
  &mDisplayGroup,DisplayCoreReady,DisplayCoreRead,DisplayCoreWrite,DisplayCoreFence,DisplayCoreDelay
};
STATIC BOOLEAN DisplayAlias(CONST VOID *A,UINTN As,CONST VOID *B,UINTN Bs)
{
  UINTN X=(UINTN)A,Y=(UINTN)B;
  return !A||!B||X>MAX_UINTN-As||Y>MAX_UINTN-Bs||(X<Y+Bs&&Y<X+As);
}
STATIC EFI_STATUS DisplayArgumentsMode(EFI_SMMU_CR_PROTOCOL *This,EFI_SMMU_CR_DISPLAY_GROUP *Group,
  BOOLEAN ReadOnly)
{
  if(This!=&mCrProtocol||!Group||DisplayAlias(Group,sizeof(*Group),&mSmmu,sizeof(mSmmu))||
     DisplayAlias(Group,sizeof(*Group),&mDisplayGroup,sizeof(mDisplayGroup))||
     DisplayAlias(Group,sizeof(*Group),mDisplayBuffers,sizeof(mDisplayBuffers)))return EFI_INVALID_PARAMETER;
  if(!mSmmu.Initialized||mSmmu.ExistingIoMmu||
     (mSmmu.AfterExitBootServices&&!(ReadOnly&&mSmmu.ExitPreparing)))return EFI_UNSUPPORTED;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS DisplayArguments(EFI_SMMU_CR_PROTOCOL *This,EFI_SMMU_CR_DISPLAY_GROUP *Group)
{ return DisplayArgumentsMode(This,Group,FALSE); }
STATIC EFI_STATUS DisplayRoutesLocked(BOOLEAN Empty)
{
  if(!DisplayCurrent()||mDisplayGroup.NextAttach>4)return EFI_NOT_READY;
  mDisplayGroup.RetirementCheck=TRUE;
  EFI_STATUS Retired=DisplayExact((EFI_STATUS)CrSmmuDisplayGroupCheckRetired(&mDisplayGroup.Core));
  mDisplayGroup.RetirementCheck=FALSE;if(Retired!=EFI_SUCCESS)return Retired;
  for(UINT32 N=0;N<mSmmu.Device.StreamCount;++N){
    UINT32 Smr=mSmmu.Device.Io.Read32(mSmmu.Device.Io.Cookie,mSmmu.Device.Base+0x800+N*4);
    if(!(Smr&BIT31))continue;
    UINT32 Mask=(Smr>>16)&0x7fffU;
    for(UINT32 I=0;I<4;++I)if(!((mDisplayGroup.TargetIdentity.Streams[I]^Smr)&~Mask&0xffffU)){
      CR_SMMU_DOMAIN *D=FindDomainBySid(mDisplayGroup.TargetIdentity.Streams[I]);
      if(I>=mDisplayGroup.NextAttach||!D||D->Domain.Stream!=N||Smr!=(BIT31|D->Sid))return EFI_NOT_READY;
    }
  }
  for(UINT32 I=0;I<mDisplayGroup.NextAttach;++I){
    CR_SMMU_DOMAIN *D=FindDomainBySid(mDisplayGroup.TargetIdentity.Streams[I]);
    if(!D||D->Poisoned||D->Segment||D->Bdf!=CR_SMMU_BDF_UNKNOWN||
       D->Domain.Device!=&mSmmu.Device||!OwnedStoppedDomainValid(D))return EFI_NOT_READY;
    if(Empty){SmmuMappingEvidence E;EFI_STATUS S=DisplayExact((EFI_STATUS)SmmuQueryRevokedMapping(
      &D->Domain,SMMU_IOVA_BASE,SMMU_IOVA_SIZE/SMMU_PAGE_SIZE,&E));if(S!=EFI_SUCCESS)return S;}
  }
  return EFI_SUCCESS;
}
STATIC BOOLEAN DisplayTerminalFailure(VOID)
{
  if(mDisplayGroup.Core.Failed||mSmmu.Device.SyncFailed)return TRUE;
  for(UINT32 I=0;I<CR_SMMU_MAX_DOMAINS;++I){
    CR_SMMU_DOMAIN *D=&mSmmu.Domains[I];
    if(DisplaySid(D->Sid)&&(D->Poisoned||D->Domain.Failed))return TRUE;
  }
  return FALSE;
}
STATIC VOID DisplayInventoryDiagnostic(EFI_STATUS Status)
{
  CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S=&mDisplayGroup.Core.Before;
  DEBUG ((DEBUG_INIT,
    "Crane SMMU: display inventory ID0=%08x ID1=%08x ID2=%08x SCR0=%08x routes=%u banks=%u status=%r\n",
    S->Id0,S->Id1,S->Id2,S->Control,S->StreamCount,S->BankCount,Status));
  for(UINT32 I=0;I<S->StreamCount&&I<4;++I){
    UINT16 N=S->Streams[I];if(N>=CR_SMMU_DISPLAY_GROUP_CORE_MAX_STREAMS)continue;
    UINT16 Bank=(UINT16)(S->S2cr[N]&0xffU);
    DEBUG ((DEBUG_INIT,"Crane SMMU: display inherited SMR[%u]=%08x S2CR=%08x CB=%u\n",
      N,S->Smr[N],S->S2cr[N],Bank));
    if(Bank>=mSmmu.Device.BankCount||Bank>=CR_SMMU_DISPLAY_GROUP_CORE_MAX_BANKS)continue;
    CR_SMMU_DISPLAY_GROUP_CORE_BANK *K=&S->Banks[Bank];
    DEBUG ((DEBUG_INIT,
      "Crane SMMU: display inherited CB[%u] SCTLR=%08x CBAR=%08x CBA2R=%08x TCR=%08x TCR2=%08x TTBR0=%Lx TTBR1=%Lx CONTEXTIDR=%08x\n",
      Bank,K->Sctlr,K->Cbar,K->Cba2r,K->Tcr,K->Tcr2,K->Ttbr0,K->Ttbr1,K->ContextId));
  }
  if(S->Conflict!=CrSmmuDisplayNoConflict){
    STATIC CONST CHAR8 *Names[]={"none","reserved stream","reserved bank",
      "reserved ASID","foreign stream shares bank","foreign ASID"};
    UINT16 N=S->ConflictIndex,Bank=N;
    DEBUG((DEBUG_ERROR,"Crane SMMU: display conflict %a index=%u ASID=%u\n",
      S->Conflict<ARRAY_SIZE(Names)?Names[S->Conflict]:"unknown",N,S->ConflictAsid));
    if(S->Conflict==CrSmmuDisplayReservedStream||S->Conflict==CrSmmuDisplaySharedBank){
      if(N>=mSmmu.Device.StreamCount)return;
      Bank=(UINT16)(S->S2cr[N]&0xffU);
      DEBUG((DEBUG_ERROR,"Crane SMMU: display conflict SMR[%u]=%08x S2CR=%08x\n",
        N,S->Smr[N],S->S2cr[N]));
    }
    if(Bank<mSmmu.Device.BankCount){
      CR_SMMU_DISPLAY_GROUP_CORE_BANK *K=&S->Banks[Bank];
      DEBUG((DEBUG_ERROR,"Crane SMMU: display conflict CB[%u] SCTLR=%08x CBAR=%08x CBA2R=%08x TCR=%08x TCR2=%08x TTBR0=%Lx TTBR1=%Lx CONTEXTIDR=%08x\n",
        Bank,K->Sctlr,K->Cbar,K->Cba2r,K->Tcr,K->Tcr2,K->Ttbr0,K->Ttbr1,K->ContextId));
    }
  }
}
STATIC EFI_STATUS EFIAPI CrSmmuAcquireDisplayGroup(EFI_SMMU_CR_PROTOCOL *This,
  UINT32 Soc,UINT64 OwnerGeneration,EFI_SMMU_CR_DISPLAY_VERIFY_STOPPED VerifyStopped,
  VOID *Context,EFI_SMMU_CR_DISPLAY_GROUP *Group)
{
  EFI_STATUS S=DisplayArguments(This,Group);if(S!=EFI_SUCCESS)return S;
  CONST CR_MDSS_TARGET *T=CrDalGetMdssContext();
  if(!OwnerGeneration||!VerifyStopped||!Context)return EFI_INVALID_PARAMETER;
  if(!DisplayTargetValid(T,Soc))return EFI_UNSUPPORTED;
  EfiAcquireLock(&mSmmu.Lock);
  if(mDisplayGroup.Active){
    if(!DisplayGroupOwner(Group)||mDisplayGroup.OwnerGeneration!=OwnerGeneration||
       mDisplayGroup.VerifyStopped!=VerifyStopped||mDisplayGroup.Context!=Context){S=EFI_ACCESS_DENIED;goto Done;}
    if(mDisplayGroup.Failed){S=EFI_DEVICE_ERROR;goto Done;}
    if(mDisplayGroup.Busy||!DisplayCurrent()){S=EFI_NOT_READY;goto Done;}
  }else{
    if(Group->Token||Group->Generation){S=EFI_INVALID_PARAMETER;goto Done;}
    for(UINT32 I=0;I<CR_SMMU_MAX_DOMAINS;++I){CR_SMMU_DOMAIN *D=&mSmmu.Domains[I];
      if(DisplaySid(D->Sid)&&(D->InUse||D->Poisoned||D->Tables)){S=EFI_ACCESS_DENIED;goto Done;}}
    if(mDisplayGeneration==MAX_UINT64){S=EFI_OUT_OF_RESOURCES;goto Done;}
    ZeroMem(&mDisplayGroup,sizeof(mDisplayGroup));mDisplayGroup.Active=TRUE;
    mDisplayGroup.Target=T;mDisplayGroup.TargetIdentity=*T;mDisplayGroup.SmmuTarget=mSmmu.Target;
    mDisplayGroup.Client=Group;mDisplayGroup.Generation=++mDisplayGeneration;
    mDisplayGroup.OwnerGeneration=OwnerGeneration;mDisplayGroup.VerifyStopped=VerifyStopped;mDisplayGroup.Context=Context;
    Group->Token=&mDisplayGroup;Group->Generation=mDisplayGroup.Generation;
    S=(EFI_STATUS)CrSmmuDisplayGroupInit(&mDisplayGroup.Core,&mSmmu.Device,
      &mDisplayCoreIo,T->Streams,mDisplayGroup.Generation);if(S!=EFI_SUCCESS)goto Done;
  }
  mDisplayGroup.Busy=TRUE;EfiReleaseLock(&mSmmu.Lock);
  S=DisplayExact(VerifyStopped(Context,OwnerGeneration));EfiAcquireLock(&mSmmu.Lock);
  if(S!=EFI_SUCCESS)goto BusyDone;
  if(!DisplayCurrent()||mSmmu.AfterExitBootServices){S=EFI_NOT_READY;goto BusyDone;}
  if(mDisplayGroup.Complete){S=DisplayRoutesLocked(FALSE);goto BusyDone;}
  if(!mDisplayGroup.Core.Inventoried){
    S=(EFI_STATUS)CrSmmuDisplayGroupObserve(&mDisplayGroup.Core);
    DisplayInventoryDiagnostic(S);if(S!=EFI_SUCCESS)goto BusyDone;
  }
  S=(EFI_STATUS)CrSmmuDisplayGroupRetire(&mDisplayGroup.Core);if(S!=EFI_SUCCESS)goto BusyDone;
  S=DisplayRoutesLocked(TRUE);if(S!=EFI_SUCCESS)goto BusyDone;
  for(;mDisplayGroup.NextAttach<4;++mDisplayGroup.NextAttach){
    EfiReleaseLock(&mSmmu.Lock);S=DisplayExact(VerifyStopped(Context,OwnerGeneration));EfiAcquireLock(&mSmmu.Lock);
    if(S!=EFI_SUCCESS||!DisplayCurrent())goto BusyDone;
    S=DisplayRoutesLocked(TRUE);if(S!=EFI_SUCCESS)goto BusyDone;
    S=AttachDisplayStreamLocked(mDisplayGroup.TargetIdentity.Streams[mDisplayGroup.NextAttach]);
    if(S!=EFI_SUCCESS)goto BusyDone;
  }
  S=DisplayRoutesLocked(TRUE);if(S!=EFI_SUCCESS)goto BusyDone;
  EfiReleaseLock(&mSmmu.Lock);S=DisplayExact(VerifyStopped(Context,OwnerGeneration));EfiAcquireLock(&mSmmu.Lock);
  if(S==EFI_SUCCESS&&DisplayCurrent())mDisplayGroup.Complete=TRUE;
  else if(S==EFI_SUCCESS)S=EFI_NOT_READY;
BusyDone:
  /* A transient stop/allocator/checked-write failure retains this exact
   * owner and its pre-effect journal. The same owner may retry only after a
   * fresh physical stop and full stage-aware hardware revalidation. A TLB
   * timeout or poisoned new domain is terminal and can never be retried. */
  if(S!=EFI_SUCCESS&&DisplayTerminalFailure())mDisplayGroup.Failed=TRUE;
  mDisplayGroup.Busy=FALSE;
Done:
  DEBUG ((EFI_ERROR(S)?DEBUG_ERROR:DEBUG_INIT,
    "Crane SMMU: display group soc=%x owner=%Lu stage=%u index=%u step=%u attached=%u complete=%u retained-failure=%u status=%r\n",
    Soc,OwnerGeneration,mDisplayGroup.Core.Stage,mDisplayGroup.Core.Index,mDisplayGroup.Core.Step,
    mDisplayGroup.NextAttach,mDisplayGroup.Complete,mDisplayGroup.Failed,S));
  mDisplayGroup.LastStatus=S;EfiReleaseLock(&mSmmu.Lock);return S;
}
