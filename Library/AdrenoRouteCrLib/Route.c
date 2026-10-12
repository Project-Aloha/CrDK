/* Standard ACPI IORT route decoder, HDK8450 execution profile. MIT */
#include <Library/AdrenoRouteCrLib.h>
static UINTN Length(CONST CHAR8 *S){UINTN N=0;while(S[N])++N;return N;}
static INT32 Compare(CONST VOID *A,CONST VOID *B,UINTN N){CONST UINT8 *P=A,*Q=B;for(UINTN I=0;I<N;++I)if(P[I]!=Q[I])return (INT32)P[I]-(INT32)Q[I];return 0;}
#define MAX_NODES 64U
#define MAX_MAPS 128U
typedef struct {UINT32 At,Bytes,Maps,MapAt,Body,Id;UINT8 Kind,Revision;} NODE;
static CONST UINT32 Irqs[25]={710,711,712,713,714,715,716,717,718,719,454,508,606,607,608,609,691,693,696,697,698,700,701,731,732};
static CONST UINT32 CpSids[4]={0,1,0x400,0x401},GmuSids[2]={5,0x405};
static UINT16 U16(CONST UINT8 *P){return (UINT16)(P[0]|((UINT16)P[1]<<8));}
static UINT32 U32(CONST UINT8 *P){return (UINT32)P[0]|((UINT32)P[1]<<8)|((UINT32)P[2]<<16)|((UINT32)P[3]<<24);}
static UINT64 U64(CONST UINT8 *P){return U32(P)|((UINT64)U32(P+4)<<32);}
static BOOLEAN Range(UINTN P,UINTN N){return P&&N&&P<=(UINTN)-1-N;}
static BOOLEAN Separate(CONST VOID *P,UINTN N,CONST VOID *Q,UINTN M){return Range((UINTN)P,N)&&Range((UINTN)Q,M)&&!((UINTN)P<(UINTN)Q+M&&(UINTN)Q<(UINTN)P+N);}
static BOOLEAN Zero(CONST UINT8 *P,UINT32 N){for(UINT32 I=0;I<N;++I)if(P[I])return FALSE;return TRUE;}
UINT32 CrAdrenoRouteCrc32(CONST VOID *V,UINT32 N){
 CONST UINT8 *P=V;UINT32 C=~0U;if(!Range((UINTN)V,N))return 0;
 for(UINT32 I=0;I<N;++I){C^=P[I];for(UINT32 B=0;B<8;++B)C=(C>>1)^((0U-(C&1U))&0xedb88320U);}return ~C;
}
static BOOLEAN MapGroup(CONST CR_ADRENO_ROUTE_MAPPING *M,CONST UINT32 *S,UINT32 N){
 UINT32 Base=M[0].InputId;if(Base>CR_MAX_UINT32-(N-1))return FALSE;
 for(UINT32 I=0;I<N;++I)if(M[I].InputId!=Base+I||M[I].StreamId!=S[I])return FALSE;
 return TRUE;
}
CR_STATUS CrAdrenoRouteValidateFacts(CONST CR_ADRENO_ROUTE_FACTS *F){
 if(!F||F->Version!=1||F->Size!=sizeof(*F)||F->IortBytes<48||F->IortBytes>CR_ADRENO_ROUTE_MAX_IORT||
 F->SmmuOffset<48||F->SmmuOffset>F->IortBytes-60||F->SmmuBase!=CR_ADRENO_ROUTE_SMMU_BASE||
 F->SmmuBytes!=CR_ADRENO_ROUTE_SMMU_BYTES||F->GlobalIrq!=705||F->ContextCount!=25||F->Reserved||
 !MapGroup(F->Cp,CpSids,4)||!MapGroup(F->Gmu,GmuSids,2))return CR_INVALID_PARAMETER;
 for(UINT32 I=0;I<25;++I)if(F->ContextIrqs[I]!=Irqs[I])return CR_UNSUPPORTED;
 return CR_SUCCESS;
}
CR_STATUS CrAdrenoRouteCheckIds(CONST CR_ADRENO_ROUTE_FACTS *F,BOOLEAN Gmu,CONST UINT32 *Ids,UINT32 Count){
 CR_STATUS S=CrAdrenoRouteValidateFacts(F);if(S!=CR_SUCCESS)return S;
 if(Gmu>1||!Ids||Count!=(Gmu?2U:4U))return CR_INVALID_PARAMETER;
 CONST CR_ADRENO_ROUTE_MAPPING *M=Gmu?F->Gmu:F->Cp;UINT32 Mask=0;
 for(UINT32 I=0;I<Count;++I){UINT32 J=0;for(;J<Count;++J)if(Ids[I]==M[J].InputId)break;
  if(J==Count||(Mask&(1U<<J)))return CR_UNSUPPORTED;Mask|=1U<<J;}
 return Mask==((1U<<Count)-1)?CR_SUCCESS:CR_UNSUPPORTED;
}
static INT32 Find(CONST NODE *N,UINT32 Count,UINT32 At){for(UINT32 I=0;I<Count;++I)if(N[I].At==At)return (INT32)I;return -1;}
static BOOLEAN Name(CONST UINT8 *B,CONST NODE *N,CONST CHAR8 *Text){
 UINT32 L=(UINT32)Length(Text);return N->Kind==1&&L+1<=N->Body-29&&
 Compare(B+N->At+29,Text,L+1)==0;
}
static BOOLEAN Arrays(CONST UINT8 *P,CONST NODE *N){
 UINT32 Starts[3]={U32(P+40),U32(P+48),U32(P+56)};
 UINT32 Counts[3]={2,U32(P+44),U32(P+52)},Ends[3]={0};
 for(UINT32 I=0;I<3;++I){UINT32 A=Starts[I],C=Counts[I];
  if(!C){if(A&& (A<60||A>N->Body))return FALSE;continue;}
  if(A<60||A>N->Body||C>(N->Body-A)/8)return FALSE;Ends[I]=A+C*8;
  for(UINT32 J=0;J<I;++J)if(Ends[J]&&A<Ends[J]&&Starts[J]<Ends[I])return FALSE;
  for(UINT32 J=0;J<C;++J){UINT32 Irq=U32(P+A+8*J),Flags=U32(P+A+8*J+4);
   if((Irq&&Irq<32)||(Flags&~1U))return FALSE;}
 }return TRUE;
}
static BOOLEAN Alias(UINT32 Sid){for(UINT32 I=0;I<4;++I)if(CpSids[I]==Sid)return TRUE;return Sid==5||Sid==0x405;}
CR_STATUS CrAdrenoRouteParseIort(CONST VOID *V,UINT32 Bytes,CR_ADRENO_ROUTE_FACTS *Out){
 if(!Separate(V,Bytes,Out,sizeof(*Out))||Bytes<48||Bytes>CR_ADRENO_ROUTE_MAX_IORT)return CR_INVALID_PARAMETER;
 CONST UINT8 *B=V;UINT8 Sum=0;for(UINT32 I=0;I<Bytes;++I)Sum=(UINT8)(Sum+B[I]);
 if(Compare(B,"IORT",4)||U32(B+4)!=Bytes||B[8]!=5||Sum||U32(B+44))return CR_UNSUPPORTED;
 UINT32 Count=U32(B+36),At=U32(B+40);NODE N[MAX_NODES];UINT32 TotalMaps=0;
 if(!Count||Count>MAX_NODES||At<48||At>Bytes||!Zero(B+48,At-48))return CR_UNSUPPORTED;
 for(UINT32 I=0;I<Count;++I){
  if(At>Bytes-16)return CR_UNSUPPORTED;CONST UINT8 *P=B+At;
  N[I]=(NODE){At,U16(P+1),U32(P+8),U32(P+12),0,U32(P+4),P[0],P[3]};NODE *Q=&N[I];
  if(Q->Bytes<16||Q->Bytes>Bytes-At||Q->Maps>MAX_MAPS-TotalMaps)return CR_UNSUPPORTED;
  TotalMaps+=Q->Maps;if(Q->Maps){if(Q->MapAt<16||Q->MapAt>Q->Bytes||Q->Maps>(Q->Bytes-Q->MapAt)/20)return CR_UNSUPPORTED;Q->Body=Q->MapAt;}
  else {if(Q->MapAt)return CR_UNSUPPORTED;Q->Body=Q->Bytes;}
  for(UINT32 J=0;J<I;++J)if(N[J].Id==Q->Id)return CR_UNSUPPORTED;
  if(Q->Kind==0){if(Q->Revision||Q->Maps||Q->Body<20||!U32(P+16)||U32(P+16)>(Q->Body-20)/4)return CR_UNSUPPORTED;}
  else if(Q->Kind==1){if(Q->Revision!=4||Q->Body<30)return CR_UNSUPPORTED;
   UINT32 End=29;while(End<Q->Body&&P[End]){if(P[End]<32||P[End]>126)return CR_UNSUPPORTED;++End;}
   if(End==29||End==Q->Body||!Zero(P+End,Q->Body-End))return CR_UNSUPPORTED;
   for(UINT32 J=0;J<I;++J)if(N[J].Kind==1&&Name(B,&N[J],(CONST CHAR8 *)P+29))return CR_UNSUPPORTED;
  }else if(Q->Kind==3){if(Q->Revision!=3||Q->Body<60||!Arrays(P,Q))return CR_UNSUPPORTED;}
  else return CR_UNSUPPORTED;
  if(Q->Maps&&!Zero(P+Q->MapAt+Q->Maps*20,Q->Bytes-Q->MapAt-Q->Maps*20))return CR_UNSUPPORTED;
  At+=Q->Bytes;
 }
 if(At!=Bytes)return CR_UNSUPPORTED;
 INT32 Cp=-1,Gmu=-1,Smmu=-1;
 for(UINT32 I=0;I<Count;++I){if(Name(B,&N[I],CR_ADRENO_ROUTE_CP_PATH))Cp=(INT32)I;
  if(Name(B,&N[I],CR_ADRENO_ROUTE_GMU_PATH))Gmu=(INT32)I;
  if(N[I].Kind==3&&U64(B+N[I].At+16)==CR_ADRENO_ROUTE_SMMU_BASE){if(Smmu>=0)return CR_UNSUPPORTED;Smmu=(INT32)I;}
  for(UINT32 J=0;J<N[I].Maps;++J){CONST UINT8 *M=B+N[I].At+N[I].MapAt+20*J;INT32 Ref=Find(N,Count,U32(M+12));
   if(Ref<0||U32(M+16)>1||U32(M)>CR_MAX_UINT32-U32(M+4)||U32(M+8)>CR_MAX_UINT32-U32(M+4))return CR_UNSUPPORTED;
   if(!((N[I].Kind==1&&(N[Ref].Kind==3||N[Ref].Kind==0))||(N[I].Kind==3&&N[Ref].Kind==0)))return CR_UNSUPPORTED;
   /* Permitted kind direction is acyclic. No recursive graph traversal. */
   for(UINT32 K=0;K<J;++K){CONST UINT8 *P=B+N[I].At+N[I].MapAt+20*K;
    if(U32(M)<=U32(P)+U32(P+4)&&U32(P)<=U32(M)+U32(M+4))return CR_UNSUPPORTED;}
  }
 }
 if(Cp<0||Gmu<0||Smmu<0)return CR_NOT_FOUND;
 CONST NODE *S=&N[Smmu];CONST UINT8 *P=B+S->At;
 if(S->Maps||U64(P+24)!=CR_ADRENO_ROUTE_SMMU_BYTES||U32(P+32)!=3||U32(P+36)!=2||
 U32(P+44)!=25||U32(P+52)!=0)return CR_UNSUPPORTED;
 UINT32 G=U32(P+40),C=U32(P+48);
 if(U32(P+G)!=705||U32(P+G+4)||U32(P+G+8)||U32(P+G+12))return CR_UNSUPPORTED;
 CR_ADRENO_ROUTE_FACTS F={0};F.Version=1;F.Size=sizeof(F);F.IortBytes=Bytes;F.IortCrc32=CrAdrenoRouteCrc32(B,Bytes);
 F.SmmuOffset=S->At;F.SmmuIdentifier=S->Id;F.SmmuBase=CR_ADRENO_ROUTE_SMMU_BASE;F.SmmuBytes=CR_ADRENO_ROUTE_SMMU_BYTES;F.GlobalIrq=705;F.ContextCount=25;
 for(UINT32 I=0;I<25;++I){F.ContextIrqs[I]=U32(P+C+I*8);if(F.ContextIrqs[I]!=Irqs[I]||U32(P+C+I*8+4))return CR_UNSUPPORTED;}
 for(UINT32 Group=0;Group<2;++Group){NODE *Q=&N[Group?Gmu:Cp];CR_ADRENO_ROUTE_MAPPING *Dest=Group?F.Gmu:F.Cp;CONST UINT32 *Sids=Group?GmuSids:CpSids;UINT32 Num=Group?2:4,Seen=0;
  P=B+Q->At;if(Q->Maps!=Num||U32(P+16)||U32(P+20)||!Zero(P+24,4)||P[28]!=36)return CR_UNSUPPORTED;
  for(UINT32 J=0;J<Num;++J){CONST UINT8 *M=P+Q->MapAt+J*20;
   if(U32(M+4)||U32(M+12)!=S->At||U32(M+16))return CR_UNSUPPORTED;
   UINT32 K=0;while(K<Num&&Sids[K]!=U32(M+8))++K;if(K==Num||(Seen&(1U<<K)))return CR_UNSUPPORTED;
   Seen|=1U<<K;Dest[K]=(CR_ADRENO_ROUTE_MAPPING){U32(M),U32(M+8)};
  }
  if(!MapGroup(Dest,Sids,Num))return CR_UNSUPPORTED;
 }
 /* No second named component may share any part of these physical routes. */
 for(UINT32 I=0;I<Count;++I)if((INT32)I!=Cp&&(INT32)I!=Gmu)for(UINT32 J=0;J<N[I].Maps;++J){
  CONST UINT8 *M=B+N[I].At+N[I].MapAt+20*J;if(U32(M+12)!=S->At)continue;
  UINT32 First=U32(M+8),Last=First+U32(M+4);
  for(UINT32 Sid=0;Sid<=0x405;++Sid)if(Alias(Sid)&&First<=Sid&&Sid<=Last)return CR_UNSUPPORTED;
 }
 if(CrAdrenoRouteValidateFacts(&F)!=CR_SUCCESS)return CR_UNSUPPORTED;*Out=F;return CR_SUCCESS;
}
