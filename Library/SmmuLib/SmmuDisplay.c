/* Public Linux arm-smmu/io-pgtable-arm based read-only display observer.
 * Only the four HDK nonsecure display SIDs are admitted. No ownership,
 * cache policy, context bank, table, global register or fault status changes.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/SmmuDisplayCrLib.h>
#define BIT(N) (1U << (N))
#define ADDRESS_MASK 0x0000fffffffff000ULL
static VOID Zero(VOID *P,UINTN N) {UINT8 *B=P;while(N--)*B++=0;}
static BOOLEAN Same(CONST VOID *A,CONST VOID *B,UINTN N) {
  CONST UINT8 *X=A,*Y=B;while(N--)if(*X++!=*Y++)return FALSE;return TRUE;
}
static UINT32 Read(SmmuDevice *D,UINT64 A){return D->Io.Read32(D->Io.Cookie,A);}
static CR_STATUS Read64(SmmuDevice *D,UINT64 A,UINT64 *V) {
  for(UINT32 I=0;I<2;++I){UINT32 H=Read(D,A+4),L=Read(D,A);
    if(H==Read(D,A+4)){*V=((UINT64)H<<32)|L;return CR_SUCCESS;}}
  return CR_NOT_READY;
}
static CR_STATUS Snapshot(SmmuDevice *D,CR_SMMU_DISPLAY_OBSERVATION *O) {
  UINT32 Covered=0;CONST UINT16 Sids[]={0x820,0x822,0xc20,0xc22};
  Zero(O,sizeof(*O));
  for(UINT32 I=0;I<D->StreamCount;++I){
    UINT32 Smr=Read(D,D->Base+0x800+I*4);
    if(!(Smr&BIT(31)))continue;
    UINT32 Mask=(Smr>>16)&0x7fff,Id=Smr&0x7fff,Match=0;
    for(UINT32 J=0;J<4;++J)if(!((Sids[J]^Id)&~Mask))Match|=BIT(J);
    if(!Match)continue;
    if(D->ReservedStreams[I]||D->InheritedStreams[I])return CR_BUSY;
    if((Smr&BIT(15))||(Mask&~0x402U)||(Id&~0x402U)!=0x820||
       (Covered&Match)||O->Count==4)return CR_UNSUPPORTED;
    CR_SMMU_DISPLAY_STREAM *S=&O->Streams[O->Count++];
    S->Smr=Smr;S->Index=(UINT16)I;S->S2cr=Read(D,D->Base+0xc00+I*4);
    UINT32 Type=(S->S2cr>>16)&3;
    if(Type>1)return CR_UNSUPPORTED;
    if(Type==0){
      S->Bank=(UINT16)(S->S2cr&0xff);
      if(S->Bank>=D->BankCount)return CR_UNSUPPORTED;
      if(D->ReservedBanks[S->Bank]||D->PhysicalExitBanks[S->Bank])return CR_BUSY;
      UINT64 Cb=D->ContextBase+(UINT64)S->Bank*D->PageSize;
      S->Cbar=Read(D,D->Base+D->PageSize+S->Bank*4);
      S->Cba2r=Read(D,D->Base+D->PageSize+0x800+S->Bank*4);
      if(((S->Cbar>>16)&3)!=1)return CR_UNSUPPORTED;
      S->Sctlr=Read(D,Cb);S->Actlr=Read(D,Cb+4);
      S->Tcr2=Read(D,Cb+0x10);S->Tcr=Read(D,Cb+0x30);
      CR_STATUS R=Read64(D,Cb+0x20,&S->Ttbr0);if(R!=CR_SUCCESS)return R;
      R=Read64(D,Cb+0x28,&S->Ttbr1);if(R!=CR_SUCCESS)return R;
      /* Every CB user must belong exclusively to this exact display set. */
      for(UINT32 J=0;J<D->StreamCount;++J){
        UINT32 Other=Read(D,D->Base+0x800+J*4);
        if(!(Other&BIT(31)))continue;
        UINT32 Route=Read(D,D->Base+0xc00+J*4);
        if(((Route>>16)&3)==0 && (Route&0xff)==S->Bank &&
           (((Other>>16)&0x7fff)&~0x402U ||
            ((Other&0x7fff)&~0x402U)!=0x820 || (Other&BIT(15))))
          return CR_UNSUPPORTED;
      }
    }
    Covered|=Match;
  }
  return Covered==15?CR_SUCCESS:CR_NOT_FOUND;
}
static CR_STATUS Translate(SmmuDevice *D,CONST CR_SMMU_DISPLAY_STREAM *S,UINT64 Iova,
    CR_SMMU_DISPLAY_TABLE_READ ReadTable,VOID *Context,UINT64 *Physical) {
  if(((S->S2cr>>16)&3)==1 || !(S->Sctlr&1)){*Physical=Iova;return CR_SUCCESS;}
  /* AArch64, TTBR0-only, 4 KiB long descriptors. Unsupported table formats
   * remain diagnostic failures rather than fabricated physical mappings. */
  if((S->Sctlr&BIT(4))||!(S->Cba2r&1)||!(S->Tcr2&BIT(4))||(S->Tcr&(BIT(7)|(3U<<14)))||
      !(S->Tcr&BIT(23))||!ReadTable)return CR_UNSUPPORTED;
  CONST UINT8 Sizes[]={32,36,40,42,44,48};
  UINT32 Encoding=S->Tcr2&15,Sep=(S->Tcr2>>15)&7;
  if(Encoding>=6||(Sep!=0 && Sep!=7)||Sizes[Encoding]>D->Stage1OutputAddressBits)
    return CR_UNSUPPORTED;
  UINT32 VaBits=64-(S->Tcr&63);
  if(VaBits<32||VaBits>48||VaBits>D->InputAddressBits||Iova>=(1ULL<<VaBits))return CR_UNSUPPORTED;
  UINT32 Level=4-((VaBits-12+8)/9);
  UINT64 Table=S->Ttbr0&ADDRESS_MASK;
  if(!Table||(S->Ttbr0&0xfffU)||Table>=(1ULL<<D->AddressBits))return CR_UNSUPPORTED;
  for(;Level<4;++Level){
    UINT32 Shift=12+9*(3-Level);
    UINT64 Pte=0,Again=0,Entry=Table+((Iova>>Shift)&511)*8;
    CR_STATUS R=ReadTable(Context,Entry,&Pte);
    if(R!=CR_SUCCESS)return CR_ERROR(R)?R:CR_NOT_READY;
    R=ReadTable(Context,Entry,&Again);
    if(R!=CR_SUCCESS)return CR_ERROR(R)?R:CR_NOT_READY;
    if(Pte!=Again)return CR_NOT_READY;
    if(Pte&0x000f000000000000ULL)return CR_UNSUPPORTED;
    UINT32 Type=(UINT32)Pte&3;
    if(Level<3 && Type==3){
      if((Pte&(1ULL<<61))||!(Pte&ADDRESS_MASK))return CR_UNSUPPORTED;
      Table=Pte&ADDRESS_MASK;
      if(Table>=(1ULL<<D->AddressBits))return CR_UNSUPPORTED;
      continue;
    }
    if((Level==3 && Type!=3)||(Level<3 && Type!=1)||Level==0||!(Pte&(1ULL<<10)))
      return CR_UNSUPPORTED;
    UINT64 OffsetMask=(1ULL<<Shift)-1,Base=Pte&ADDRESS_MASK;
    if(Base&OffsetMask)return CR_UNSUPPORTED;
    *Physical=Base|(Iova&OffsetMask);
    return *Physical>=(1ULL<<Sizes[Encoding])?CR_UNSUPPORTED:CR_SUCCESS;
  }
  return CR_UNSUPPORTED;
}
CR_STATUS CrSmmuDisplayObserve(SmmuDevice *D,UINT64 Device,UINT64 Bytes,
    UINT64 ReservedBase,UINT64 ReservedBytes,CR_SMMU_DISPLAY_TABLE_READ ReadTable,
    VOID *Context,CR_SMMU_DISPLAY_OBSERVATION *Output) {
  if(!Output)return CR_INVALID_PARAMETER;
  Zero(Output,sizeof(*Output));
  if(!D||D->ProbeState!=SmmuProbeReady||!D->Io.Read32||!D->Base||
     !D->ContextBase||(D->PageSize!=4096 && D->PageSize!=65536)||
     !D->StreamCount||D->StreamCount>128||!D->BankCount||D->BankCount>256||
     D->AddressBits<32||D->AddressBits>48||D->Stage1OutputAddressBits<32||
     D->Stage1OutputAddressBits>48||!Device||!Bytes||Bytes>0x2300000||Device>0xffffffffULL||
     Bytes>0x100000000ULL-Device||!ReservedBytes||
     ReservedBase>MAX_UINT64-ReservedBytes)return CR_INVALID_PARAMETER;
  CR_SMMU_DISPLAY_OBSERVATION Before,After;
  UINT32 Control=Read(D,D->Base);
  if(Control&(BIT(0)|BIT(3)))return CR_UNSUPPORTED;
  CR_STATUS R=Snapshot(D,&Before);if(R!=CR_SUCCESS)return R;
  UINT64 Physical=0;
  for(UINT32 I=0;I<Before.Count;++I){
    UINT64 First=0;R=Translate(D,&Before.Streams[I],Device,ReadTable,Context,&First);
    if(R!=CR_SUCCESS)return R;
    if(First<ReservedBase||First>=ReservedBase+ReservedBytes||
       Bytes>ReservedBase+ReservedBytes-First||
       First>=(1ULL<<D->Stage1OutputAddressBits)||
       Bytes>(1ULL<<D->Stage1OutputAddressBits)-First)return CR_UNSUPPORTED;
    if(I && First!=Physical)return CR_UNSUPPORTED;
    Physical=First;
    for(UINT64 Offset=0;Offset<Bytes;){
      UINT64 At=0;R=Translate(D,&Before.Streams[I],Device+Offset,ReadTable,Context,&At);
      if(R!=CR_SUCCESS)return R;
      if(At!=Physical+Offset)return CR_UNSUPPORTED;
      UINT64 Step=4096-((Device+Offset)&4095);
      Offset+=Step<Bytes-Offset?Step:Bytes-Offset;
    }
    UINT64 Last=0;R=Translate(D,&Before.Streams[I],Device+Bytes-1,ReadTable,Context,&Last);
    if(R!=CR_SUCCESS)return R;
    if(Last!=Physical+Bytes-1)return CR_UNSUPPORTED;
  }
  R=Snapshot(D,&After);if(R!=CR_SUCCESS)return R;
  if(Read(D,D->Base)!=Control)return CR_NOT_READY;
  if(!Same(&Before,&After,sizeof(Before)))return CR_NOT_READY;
  Before.Size=sizeof(Before);Before.Revision=1;Before.DeviceAddress=Device;
  Before.PhysicalAddress=Physical;Before.Bytes=Bytes;*Output=Before;
  return CR_SUCCESS;
}
