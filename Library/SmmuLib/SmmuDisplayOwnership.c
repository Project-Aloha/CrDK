/* Retained source-qualified display SMMU producer access.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "SmmuDisplayOwnershipInternal.h"

static CR_STATUS Exact(CR_STATUS S)
{ return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY; }
CR_STATUS BsReady(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  if(!B||!B->Initialized||B->Self!=B||!B->Device||!B->Generation)
    return CR_INVALID_PARAMETER;
  SmmuDevice *D=B->Device;
  if(D->ProbeState!=SmmuProbeReady||D->Base!=0x15000000ULL||
     D->ContextBase!=0x15080000ULL||!D->BankCount||D->BankCount>128||
     !D->StreamCount||D->StreamCount>128||D->Stage2BankCount>=D->BankCount||
     (D->PageSize!=4096&&D->PageSize!=65536)||
     (UINT64)D->BankCount*D->PageSize>0x80000ULL)return CR_UNSUPPORTED;
  if(B->Failed||D->SyncFailed)return CR_NOT_READY;
  return Exact(B->Io.Ready(B->Io.Context,B->Generation));
}
CR_STATUS BsRead(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 O,UINT32 *V)
{
  if(!V||(O&3)||O>0xffffc)return CR_INVALID_PARAMETER;
  BST(BsReady(B));return Exact(B->Io.Read32(B->Io.Context,O,V));
}
CR_STATUS BsWrite(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 O,UINT32 V)
{
  if((O&3)||O>0xffffc)return CR_INVALID_PARAMETER;
  BST(BsReady(B));
  if(!B->Inventoried||!B->Reserved||!B->Stage)return CR_NOT_READY;
  B->Touched=TRUE;return Exact(B->Io.Write32(B->Io.Context,O,V));
}
CR_STATUS BsFence(CR_SMMU_DISPLAY_GROUP_CORE *B)
{ BST(BsReady(B));return Exact(B->Io.Fence(B->Io.Context)); }
CR_STATUS BsDelay(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 Us)
{
  if(!Us||Us>1024)return CR_INVALID_PARAMETER;
  BST(BsReady(B));return Exact(B->Io.DelayUs(B->Io.Context,Us));
}
CR_STATUS BsChecked(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 O,UINT32 V,UINT32 Mask)
{
  UINT32 A;BST(BsWrite(B,O,V));BST(BsFence(B));BST(BsRead(B,O,&A));
  return ((A^V)&Mask)?CR_DEVICE_ERROR:CR_SUCCESS;
}
CR_STATUS CrSmmuDisplayGroupInit(CR_SMMU_DISPLAY_GROUP_CORE *B,SmmuDevice *D,
  CONST CR_SMMU_DISPLAY_GROUP_CORE_IO *Io,CONST UINT16 Sids[4],UINT64 Generation)
{
  if(!B||!D||!Io||!Sids||!Generation||!Io->Ready||!Io->Read32||!Io->Write32||
     !Io->Fence||!Io->DelayUs)return CR_INVALID_PARAMETER;
  if(B->Initialized||B->Reserved||B->Touched)return CR_ALREADY_STARTED;
  UINT16 Mask=0;
  for(UINT32 I=0;I<4;++I){
    if(Sids[I]>0x7fff)return CR_UNSUPPORTED;
    Mask|=Sids[I]^Sids[0];
    for(UINT32 J=0;J<I;++J)if(Sids[I]==Sids[J])return CR_INVALID_PARAMETER;
  }
  UINT32 Bits=0;for(UINT32 I=0;I<15;++I)Bits+=(Mask>>I)&1;
  if(Bits!=2)return CR_UNSUPPORTED;
  CR_SMMU_DISPLAY_GROUP_CORE_IO Saved=*Io;
  cr_memset(B,0,sizeof(*B));B->Self=B;B->Device=D;B->Io=Saved;
  B->Generation=Generation;B->SidMask=Mask;B->SidBase=Sids[0]&~Mask;
  cr_memcpy(B->Sids,Sids,sizeof(B->Sids));B->Initialized=TRUE;return CR_SUCCESS;
}
CR_STATUS CrSmmuDisplayGroupObserve(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  BST(BsReady(B));if(B->Touched||B->Reserved||B->Retired)return CR_BUSY;
  if(B->Inventoried)return CrSmmuDisplayGroupCheckInventory(B);
  B->Inventoried=FALSE;
  CR_STATUS S=BsCapture(B,&B->Before);
  if(S==CR_SUCCESS)S=BsAdmit(B,&B->Before,FALSE);
  B->Inventoried=S==CR_SUCCESS;B->LastStatus=S;return S;
}
CR_STATUS CrSmmuDisplayGroupAbort(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  if(!B||!B->Initialized||B->Self!=B)return CR_INVALID_PARAMETER;
  if(B->Touched||B->Reserved||B->Retired)return CR_BUSY;
  cr_memset(B,0,sizeof(*B));return CR_SUCCESS;
}

/* Inherited stream/context inventory, never a table or mapping grant.
 * Register semantics: public Linux arm-smmu. Stage1 formats are decoded only
 * for local ASID exclusion; inherited page tables are never read or adopted.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "SmmuDisplayOwnershipInternal.h"

#define BS_VALID 0x80000000U
#define BS_TRANS 0x00030000U
#define BS_S1 0x00010000U
#define BS_AS 0x10U

static BOOLEAN Has(CONST UINT16 *A,UINT32 Count,UINT16 Value)
{ for(UINT32 I=0;I<Count;++I)if(A[I]==Value)return TRUE;return FALSE; }
static BOOLEAN Referenced(CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S,UINT32 Count,UINT32 Bank)
{
  for(UINT32 I=0;I<Count;++I)
    if((S->Smr[I]&BS_VALID)&&!(S->S2cr[I]&BS_TRANS)&&
       (S->S2cr[I]&0xffU)==Bank)return TRUE;
  return FALSE;
}
static CR_STATUS Geometry(CONST SmmuDevice *D,CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S)
{
  static CONST UINT8 Bits[]={32,36,40,42,44,48};
  if(!D)return CR_INVALID_PARAMETER;
  UINT32 Shift=(S->Id1&BS_VALID)?16:12;
  UINT32 Pages=1U<<(((S->Id1>>28)&7U)+1U);
  UINT32 N=S->Id1&0xffU,Stage2=(S->Id1>>16)&0xffU,Streams=S->Id0&0xffU;
  UINT32 Ubs=(S->Id2>>8)&15U,Ias=S->Id2&15U,Oas=(S->Id2>>4)&15U;
  UINT32 SidBits=(S->Id0>>9)&15U;
  if((S->Id0&0x48000000U)!=0x48000000U||!(S->Id2&0x1000U)||
     Shift!=12||((UINT64)Pages<<Shift)!=0x80000ULL||
     !N||N>CR_SMMU_DISPLAY_GROUP_CORE_MAX_BANKS||N>Pages||Stage2>=N||
     !Streams||Streams>CR_SMMU_DISPLAY_GROUP_CORE_MAX_STREAMS||SidBits<12||
     Ubs>=sizeof(Bits)||Ias>=sizeof(Bits)||Oas>=sizeof(Bits)||
     D->PageSize!=(1U<<Shift)||D->BankCount!=N||D->Stage2BankCount!=Stage2||
     D->StreamCount!=Streams||D->ContextBase<D->Base||
     D->ContextBase-D->Base!=0x80000ULL||
     D->InputAddressBits!=Bits[Ubs]||D->InputAddressEncoding!=Ubs||
     D->Stage1OutputAddressBits!=Bits[Ias]||D->Stage1OutputAddressEncoding!=Ias||
     D->AddressBits!=Bits[Oas]||D->AddressEncoding!=Oas||
     D->TranslationAddressBits!=(Bits[Ubs]>39?39:Bits[Ubs]))return CR_UNSUPPORTED;
  return CR_SUCCESS;
}
static CR_STATUS Read64(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 Offset,UINT64 *Value)
{
  UINT32 High,Low,Again;
  BST(BsRead(B,Offset+4,&High));BST(BsRead(B,Offset,&Low));
  BST(BsRead(B,Offset+4,&Again));
  if(High!=Again)return CR_NOT_READY;
  *Value=((UINT64)High<<32)|Low;return CR_SUCCESS;
}
CR_STATUS BsCapture(CR_SMMU_DISPLAY_GROUP_CORE *B,CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S)
{
  /* Internal output is always one of the retained owner's two snapshots.
   * No caller-selected buffer may overwrite Device or an IO callback cookie. */
  if(!B||!S||(S!=&B->Before&&S!=&B->Scratch))return CR_INVALID_PARAMETER;
  BST(BsReady(B));
  if(B->Reserved&&(S==&B->Before||B->Before.BankCount>CR_SMMU_DISPLAY_GROUP_CORE_MAX_OWNED))
    return CR_NOT_READY;
  cr_memset(S,0,sizeof(*S));
  BST(BsRead(B,0x20,&S->Id0));BST(BsRead(B,0x24,&S->Id1));
  BST(BsRead(B,0x28,&S->Id2));BST(BsRead(B,0,&S->Control));
  BST(Geometry(B->Device,S));
  if(S->Control&BSBIT(3))return CR_UNSUPPORTED;
  for(UINT32 I=0;I<B->Device->StreamCount;++I){
    BST(BsRead(B,0x800+4*I,&S->Smr[I]));BST(BsRead(B,0xc00+4*I,&S->S2cr[I]));
  }
  for(UINT32 I=0;I<B->Device->BankCount;++I){
    CR_SMMU_DISPLAY_GROUP_CORE_BANK *K=&S->Banks[I];UINT32 Base=0x80000+I*B->Device->PageSize;
    BST(BsRead(B,Base,&K->Sctlr));
    if(!(K->Sctlr&1U)&&!Referenced(S,B->Device->StreamCount,I)&&
       !(B->Reserved&&Has(B->Before.OwnedBanks,B->Before.BankCount,(UINT16)I)))continue;
    BST(BsRead(B,B->Device->PageSize+4*I,&K->Cbar));
    BST(BsRead(B,B->Device->PageSize+0x800+4*I,&K->Cba2r));
    BST(BsRead(B,Base+4,&K->Actlr));BST(BsRead(B,Base+0x10,&K->Tcr2));
    BST(Read64(B,Base+0x20,&K->Ttbr0));BST(Read64(B,Base+0x28,&K->Ttbr1));
    BST(BsRead(B,Base+0x30,&K->Tcr));BST(BsRead(B,Base+0x34,&K->ContextId));
    BST(BsRead(B,Base+0x38,&K->Mair0));BST(BsRead(B,Base+0x3c,&K->Mair1));
  }
  UINT32 V;
  BST(BsRead(B,0x20,&V));if(V!=S->Id0)return CR_NOT_READY;
  BST(BsRead(B,0x24,&V));if(V!=S->Id1)return CR_NOT_READY;
  BST(BsRead(B,0x28,&V));if(V!=S->Id2)return CR_NOT_READY;
  BST(BsRead(B,0,&V));return V==S->Control?CR_SUCCESS:CR_NOT_READY;
}

/* Decode tags without inferring translation proof. Owned retirement requires
 * equal TTBR tags; foreign ownership uses only the ASID selected by TCR.A1.
 * Unknown/nested stages or formats fail closed rather than hiding aliases. */
static CR_STATUS EffectiveAsids(CONST CR_SMMU_DISPLAY_GROUP_CORE_BANK *K,UINT32 Id0,UINT16 Asid[2])
{
  if((K->Cbar&BS_TRANS)!=BS_S1||(K->Cba2r&0x0000fffeU))return CR_UNSUPPORTED;
  if(K->Cba2r&1U){
    /* Only AS affects TTBR ASID width. PASIZE/SEP and implementation or
     * reserved readback bits do not change that namespace. We never adopt
     * the inherited translation policy, and owned-bank rechecks preserve
     * every full register value rather than normalizing these other bits. */
    UINT32 Mask=(K->Tcr2&BS_AS)?0xffffU:0xffU;
    Asid[0]=(UINT16)((K->Ttbr0>>48)&Mask);
    Asid[1]=(UINT16)((K->Ttbr1>>48)&Mask);
  }else if(K->Tcr&BS_VALID){ /* AArch32 long descriptors: eight-bit ASID. */
    if((K->Tcr2&BS_AS)||(Id0&BSBIT(25)))return CR_UNSUPPORTED;
    Asid[0]=(UINT16)((K->Ttbr0>>48)&0xffU);
    Asid[1]=(UINT16)((K->Ttbr1>>48)&0xffU);
  }else{ /* AArch32 short descriptors use CONTEXTIDR, not TTBR tags. */
    if((K->Tcr2&BS_AS)||(Id0&(BSBIT(25)|BSBIT(24))))return CR_UNSUPPORTED;
    Asid[0]=Asid[1]=(UINT16)(K->ContextId&0xffU);
  }
  return CR_SUCCESS;
}
static CR_STATUS OwnedBank(CONST SmmuDevice *D,CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S,
                           UINT16 Bank,UINT16 *Asid)
{
  CONST CR_SMMU_DISPLAY_GROUP_CORE_BANK *K=&S->Banks[Bank];UINT16 Tags[2];
  BST(EffectiveAsids(K,S->Id0,Tags));
  /* Retirement does not translate old pages or reinterpret their cache
   * policy. Admit only exclusive stage1 banks with one decoded ASID shared
   * by both TTBR tags; stage2/nested/unknown formats fail closed. Public
   * arm-smmu permits ASID0 and invalidates that tag with local TLBIASID.
   * Qualify zero here only for a disabled, empty AArch64 bank. It still
   * reserves the old tag and undergoes the same fault/disable/TLBI/sync. */
  if(Bank<D->Stage2BankCount||K->Cbar&0x80000000U||
     Tags[0]!=Tags[1]||Tags[0]==0xffffU||
     (!Tags[0]&&((K->Sctlr&1U)||!(K->Cba2r&1U)||K->Ttbr0||K->Ttbr1)))
    return CR_UNSUPPORTED;
  *Asid=Tags[0];return CR_SUCCESS;
}
static CR_STATUS Conflict(CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S,
  CR_SMMU_DISPLAY_CONFLICT Kind,UINT16 Index,UINT16 Asid)
{
  S->Conflict=Kind;S->ConflictIndex=Index;S->ConflictAsid=Asid;return CR_BUSY;
}
static CR_STATUS ForeignAsids(CR_SMMU_DISPLAY_GROUP_CORE *B,CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S);
CR_STATUS BsAdmit(CR_SMMU_DISPLAY_GROUP_CORE *B,CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S,BOOLEAN Allow)
{
  if(!B||!B->Device||!S||Allow>1)return CR_INVALID_PARAMETER;
  if(Allow&&(!B->Reserved||S==&B->Before||
     B->Before.StreamCount>4||B->Before.BankCount>4))return CR_NOT_READY;
  S->StreamCount=S->BankCount=0;
  S->Conflict=CrSmmuDisplayNoConflict;S->ConflictIndex=S->ConflictAsid=0;
  cr_memset(S->Streams,0,sizeof(S->Streams));cr_memset(S->OwnedBanks,0,sizeof(S->OwnedBanks));
  cr_memset(S->Asids,0,sizeof(S->Asids));
  BST(Geometry(B->Device,S));
  /* Adopted globals remain byte-for-byte unchanged. Unmatched stream
   * faulting must already be enabled before any SMR can be invalidated. */
  if((S->Control&(BSBIT(0)|BSBIT(3)))||!(S->Control&BSBIT(10)))
    return CR_UNSUPPORTED;
  UINT32 Seen=0,SidMask=(1U<<((S->Id0>>9)&15U))-1U;
  for(UINT32 I=0;I<B->Device->StreamCount;++I){
    UINT32 Smr=S->Smr[I],Route=S->S2cr[I];if(!(Smr&BS_VALID))continue;
    UINT32 Id=Smr&0xffffU,Mask=(Smr>>16)&0x7fffU;
    if((Id&~SidMask)||(Mask&~SidMask)||(Route&BSBIT(10))||
       ((Route>>16)&3U)==3U)return CR_UNSUPPORTED;
    UINT32 Match=0;
    for(UINT32 J=0;J<4;++J)if(((B->Sids[J]^Id)&~Mask)==0)Match|=1U<<J;
    if(!Match)continue;
    /* A matching pattern may vary only the two board-owned SID bits. */
    if((Mask&~B->SidMask)||((Id^B->SidBase)&~B->SidMask)||
       (Seen&Match)||S->StreamCount==4||
       (Route&BS_TRANS)!=0)return CR_UNSUPPORTED;
    UINT16 Bank=(UINT16)(Route&0xffU);
    if(Bank>=B->Device->BankCount)return CR_UNSUPPORTED;
    if(B->Device->ReservedStreams[I]&&
       !(Allow&&Has(B->Before.Streams,B->Before.StreamCount,(UINT16)I)))
      return Conflict(S,CrSmmuDisplayReservedStream,(UINT16)I,0);
    S->Streams[S->StreamCount++]=(UINT16)I;Seen|=Match;
    if(!Has(S->OwnedBanks,S->BankCount,Bank)){
      if(S->BankCount==4)return CR_UNSUPPORTED;
      UINT16 Asid;BST(OwnedBank(B->Device,S,Bank,&Asid));
      if(B->Device->PhysicalExitBanks[Bank]||
         (B->Device->ReservedBanks[Bank]&&
          !(Allow&&Has(B->Before.OwnedBanks,B->Before.BankCount,Bank))))
        return Conflict(S,CrSmmuDisplayReservedBank,Bank,Asid);
      if(Asid<256&&B->Device->ReservedAsids[Asid]&&
         !(Allow&&Has(B->Before.Asids,B->Before.BankCount,Asid)))
        return Conflict(S,CrSmmuDisplayReservedAsid,Bank,Asid);
      S->OwnedBanks[S->BankCount]=Bank;S->Asids[S->BankCount++]=Asid;
    }
  }
  if(Seen&&Seen!=15)return CR_NOT_FOUND;
  /* A disabled translating CB is still a referenced owner. Foreign streams
   * cannot share an owned bank even if its current SCTLR.M were clear. */
  for(UINT32 I=0;I<B->Device->StreamCount;++I){
    if(!(S->Smr[I]&BS_VALID)||(S->S2cr[I]&BS_TRANS))continue;
    UINT16 Bank=(UINT16)(S->S2cr[I]&0xffU);
    if(Bank>=B->Device->BankCount)return CR_UNSUPPORTED;
    if(Has(S->OwnedBanks,S->BankCount,Bank)&&!Has(S->Streams,S->StreamCount,(UINT16)I))
      return Conflict(S,CrSmmuDisplaySharedBank,(UINT16)I,0);
  }
  return ForeignAsids(B,S);
}
static CR_STATUS ForeignAsids(CR_SMMU_DISPLAY_GROUP_CORE *B,CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *S)
{
  for(UINT32 I=0;I<B->Device->BankCount;++I){
    if(Has(S->OwnedBanks,S->BankCount,(UINT16)I))continue;
    if(!(S->Banks[I].Sctlr&1U)&&!Referenced(S,B->Device->StreamCount,I))continue;
    UINT16 Tags[2];BST(EffectiveAsids(&S->Banks[I],S->Id0,Tags));
    /* A referenced but disabled, empty AArch64 stage1 CB has no live
     * translation using tag zero. Local TLBIASID may evict stale tag-zero
     * entries without altering its route, bank or tables. This exception
     * is only for an unclaimed empty CB, never an active/nonempty or
     * software-owned context. Every revalidation decodes it again. */
    CONST CR_SMMU_DISPLAY_GROUP_CORE_BANK *K=&S->Banks[I];
    if(!(K->Sctlr&1U)&&(K->Cba2r&1U)&&!(K->Cbar&BS_VALID)&&
       !K->Ttbr0&&!K->Ttbr1&&!B->Device->ReservedBanks[I]&&
       !B->Device->PhysicalExitBanks[I])continue;
    /* Public Linux arm-smmu.h ARM_SMMU_TCR_A1. EPD0/EPD1 select
       table walks, not the ASID source. Short descriptors decoded above
       return CONTEXTIDR in both slots, so A1 cannot change their ASID. */
    UINT16 Asid=Tags[(S->Banks[I].Tcr>>22)&1U];
    if(Has(S->Asids,S->BankCount,Asid))
      return Conflict(S,CrSmmuDisplaySharedAsid,(UINT16)I,Asid);
  }
  return CR_SUCCESS;
}
static BOOLEAN SameBank(CONST CR_SMMU_DISPLAY_GROUP_CORE_BANK *A,CONST CR_SMMU_DISPLAY_GROUP_CORE_BANK *B)
{
  return A->Sctlr==B->Sctlr&&A->Cbar==B->Cbar&&A->Cba2r==B->Cba2r&&
    A->Tcr==B->Tcr&&A->Tcr2==B->Tcr2&&A->Mair0==B->Mair0&&A->Mair1==B->Mair1&&
    A->Actlr==B->Actlr&&A->ContextId==B->ContextId&&A->Ttbr0==B->Ttbr0&&A->Ttbr1==B->Ttbr1;
}
CR_STATUS BsSameOwned(CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *A,CONST CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *B)
{
  if(!A||!B)return CR_INVALID_PARAMETER;
  if(A->StreamCount>4||A->BankCount>4||
     A->Id0!=B->Id0||A->Id1!=B->Id1||A->Id2!=B->Id2||A->Control!=B->Control||
     A->StreamCount!=B->StreamCount||A->BankCount!=B->BankCount)return CR_NOT_READY;
  for(UINT32 I=0;I<A->StreamCount;++I){
    UINT16 N=A->Streams[I];
    if(N>=128||N!=B->Streams[I]||A->Smr[N]!=B->Smr[N]||A->S2cr[N]!=B->S2cr[N])return CR_NOT_READY;
  }
  for(UINT32 I=0;I<A->BankCount;++I){
    UINT16 N=A->OwnedBanks[I];
    if(N>=128||N!=B->OwnedBanks[I]||A->Asids[I]!=B->Asids[I]||
       !SameBank(&A->Banks[N],&B->Banks[N]))return CR_NOT_READY;
  }
  return CR_SUCCESS;
}

/* Physical stop precedes this local SMMU transaction; old pages stay owned.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "SmmuDisplayOwnershipInternal.h"

#define SMR(I) (0x800U+4U*(I))
#define S2CR(I) (0xc00U+4U*(I))
static UINT32 FaultRoute(UINT32 Old)
{ return (Old&~0x300ffU)|0x20000U; }
#define VALID 0x80000000U

static UINT32 Cb(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 I)
{ return 0x80000U+(UINT32)B->Before.OwnedBanks[I]*B->Device->PageSize; }

/* A possibly posted failing write can leave either exact old or new state.
 * Completed phases require new state exclusively; reactivation is rejected.
 * No expected normalizer hides changes to any other owned context field. */
static BOOLEAN Match(UINT32 Actual,UINT32 Old,UINT32 New,UINT32 Progress)
{ return Progress==0?Actual==Old:Progress==2?Actual==New:Actual==Old||Actual==New; }
static UINT32 Progress(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 Stage,UINT32 I)
{
  if(B->Stage<Stage)return 0;
  if(B->Stage>Stage||I<B->Index)return 2;
  return I==B->Index&&B->Step?1:0;
}
static CR_STATUS Revalidate(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *O=&B->Before,*N=&B->Scratch;
  BST(BsCapture(B,N));
  for(UINT32 I=0;I<O->StreamCount;++I){
    UINT16 S=O->Streams[I];
    if(!Match(N->S2cr[S],O->S2cr[S],FaultRoute(O->S2cr[S]),Progress(B,1,I))||
       !Match(N->Smr[S],O->Smr[S],O->Smr[S]&~VALID,Progress(B,3,I)))
      return CR_DEVICE_ERROR;
    if(B->Reserved&&!B->Device->ReservedStreams[S])return CR_NOT_READY;
    N->S2cr[S]=O->S2cr[S];N->Smr[S]=O->Smr[S];
  }
  for(UINT32 I=0;I<O->BankCount;++I){
    UINT16 C=O->OwnedBanks[I];UINT32 P=Progress(B,2,I);
    if(B->Stage==2&&I==B->Index&&B->Step>=2)P=2;
    if(!Match(N->Banks[C].Sctlr,O->Banks[C].Sctlr,0,P))return CR_DEVICE_ERROR;
    if(B->Reserved&&(!B->Device->ReservedBanks[C]||
       (O->Asids[I]<256&&!B->Device->ReservedAsids[O->Asids[I]])))return CR_NOT_READY;
    N->Banks[C].Sctlr=O->Banks[C].Sctlr;
  }
  BST(BsAdmit(B,N,B->Reserved));return BsSameOwned(O,N);
}
static CR_STATUS CheckTerminal(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  CR_SMMU_DISPLAY_GROUP_CORE_SNAPSHOT *O=&B->Before,*N=&B->Scratch;
  if(!B->Reserved||B->Stage!=4||O->StreamCount>4||O->BankCount>4)return CR_NOT_READY;
  BST(BsCapture(B,N));
  for(UINT32 I=0;I<O->StreamCount;++I){
    UINT16 S=O->Streams[I];if(S>=B->Device->StreamCount)return CR_COMPROMISED_DATA;
    if(N->Smr[S]!=(O->Smr[S]&~VALID)||N->S2cr[S]!=FaultRoute(O->S2cr[S])||
       !B->Device->ReservedStreams[S])return CR_NOT_READY;
    N->Smr[S]=O->Smr[S];N->S2cr[S]=O->S2cr[S];
  }
  for(UINT32 I=0;I<O->BankCount;++I){
    UINT16 C=O->OwnedBanks[I];if(C>=B->Device->BankCount)return CR_COMPROMISED_DATA;
    if(N->Banks[C].Sctlr||!B->Device->ReservedBanks[C]||
       (O->Asids[I]<256&&!B->Device->ReservedAsids[O->Asids[I]]))return CR_NOT_READY;
    for(UINT32 J=0;J<B->Device->StreamCount;++J){
      BOOLEAN Old=FALSE;for(UINT32 K=0;K<O->StreamCount;++K)if(J==O->Streams[K])Old=TRUE;
      if(!Old&&(N->Smr[J]&VALID)&&!(N->S2cr[J]&0x30000U)&&(N->S2cr[J]&0xffU)==C)
        return CR_NOT_READY;
    }
    N->Banks[C].Sctlr=O->Banks[C].Sctlr;
  }
  N->StreamCount=O->StreamCount;N->BankCount=O->BankCount;
  cr_memcpy(N->Streams,O->Streams,sizeof(N->Streams));
  cr_memcpy(N->OwnedBanks,O->OwnedBanks,sizeof(N->OwnedBanks));
  cr_memcpy(N->Asids,O->Asids,sizeof(N->Asids));
  BST(ForeignAsids(B,N));
  return BsSameOwned(O,N);
}
static CR_STATUS Reserve(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  BST(Revalidate(B));
  for(UINT32 I=0;I<B->Before.StreamCount;++I)
    B->Device->ReservedStreams[B->Before.Streams[I]]=TRUE;
  for(UINT32 I=0;I<B->Before.BankCount;++I){
    B->Device->ReservedBanks[B->Before.OwnedBanks[I]]=TRUE;
    if(B->Before.Asids[I]<256)B->Device->ReservedAsids[B->Before.Asids[I]]=TRUE;
  }
  B->Reserved=TRUE;B->Stage=1;return CR_SUCCESS;
}
static CR_STATUS Sync(CR_SMMU_DISPLAY_GROUP_CORE *B,UINT32 Base)
{
  /* Issue a new sync before every retry's completion observation. A fresh
   * post-command low is source-defined completion, not an old cached sample. */
  BST(BsWrite(B,Base+0x7f0,0xffffffffU));BST(BsFence(B));
  UINT32 Elapsed=0,Delay=1;
  for(;;){
    UINT32 V;BST(BsRead(B,Base+0x7f4,&V));if(!(V&1))return CR_SUCCESS;
    if(Elapsed==500000)break;
    UINT32 ThisDelay=Delay;if(ThisDelay>500000-Elapsed)ThisDelay=500000-Elapsed;
    BST(BsDelay(B,ThisDelay));Elapsed+=ThisDelay;if(Delay<1000)Delay*=2;
  }
  B->Device->SyncFailed=TRUE;B->Failed=TRUE;return CR_TIMEOUT;
}
static CR_STATUS Retire(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  BST(BsReady(B));if(!B->Inventoried)return CR_NOT_READY;
  if(B->Stage==4)return CrSmmuDisplayGroupCheckRetired(B);
  if(!B->Reserved)BST(Reserve(B));else BST(Revalidate(B));
  while(B->Stage<4){
    if(B->Stage==1){
      if(B->Index==B->Before.StreamCount){B->Stage=2;B->Index=B->Step=0;continue;}
      B->Step=1;
      BST(BsChecked(B,S2CR(B->Before.Streams[B->Index]),
        FaultRoute(B->Before.S2cr[B->Before.Streams[B->Index]]),0xffffffffU));
      ++B->Index;B->Step=0;
    }else if(B->Stage==2){
      if(B->Index==B->Before.BankCount){B->Stage=3;B->Index=B->Step=0;continue;}
      UINT32 Base=Cb(B,B->Index);
      if(B->Step<2){B->Step=1;BST(BsChecked(B,Base,0,0xffffffffU));B->Step=2;}
      if(B->Step<4){
        BST(Revalidate(B));
        B->Step=3;BST(BsWrite(B,Base+0x610,B->Before.Asids[B->Index]));
        BST(BsFence(B));B->Step=4;
      }
      BST(Sync(B,Base));++B->Index;B->Step=0;
    }else if(B->Stage==3){
      if(B->Index==B->Before.StreamCount){B->Stage=4;B->Index=B->Step=0;continue;}
      /* USFCFG was source-qualified and rechecked, so invalidating this
       * completed fault mapping cannot expose unmatched bypass. */
      UINT32 Control;BST(BsRead(B,0,&Control));
      if(Control!=B->Before.Control||!(Control&BSBIT(10)))return CR_DEVICE_ERROR;
      UINT16 I=B->Before.Streams[B->Index];B->Step=1;
      BST(BsChecked(B,SMR(I),B->Before.Smr[I]&~VALID,0xffffffffU));
      ++B->Index;B->Step=0;
    }else return CR_DEVICE_ERROR;
  }
  BST(CheckTerminal(B));B->Retired=TRUE;return CR_SUCCESS;
}
CR_STATUS CrSmmuDisplayGroupRetire(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  if(!B||!B->Initialized||B->Self!=B)return CR_INVALID_PARAMETER;
  CR_STATUS S=Retire(B);B->LastStatus=S;return S;
}
CR_STATUS CrSmmuDisplayGroupCheckRetired(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  BST(BsReady(B));if(!B->Reserved||B->Stage!=4)return CR_NOT_READY;
  CR_STATUS S=CheckTerminal(B);B->Retired=S==CR_SUCCESS;
  B->LastStatus=S;return S;
}

CR_STATUS CrSmmuDisplayGroupCheckInventory(CR_SMMU_DISPLAY_GROUP_CORE *B)
{
  BST(BsReady(B));if(!B->Inventoried||B->Reserved||B->Touched)return CR_NOT_READY;
  CR_STATUS S=Revalidate(B);B->LastStatus=S;return S;
}
