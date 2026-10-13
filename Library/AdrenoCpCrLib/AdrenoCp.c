/* Retained actual ring publication/progress. SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
#define ACTIVE 1U
STATIC UINT32 Cas(volatile UINT32 *P,UINT32 O,UINT32 V){
#ifdef _KERNEL_MODE
 return (UINT32)InterlockedCompareExchange((volatile LONG *)P,(LONG)V,(LONG)O);
#else
 return InterlockedCompareExchange32(P,O,V);
#endif
}
STATIC BOOLEAN Span(CONST VOID *P,UINTN N,UINTN A){return P&&N&&!( (UINTN)P%A )&&(UINTN)P<=~(UINTN)0-N;}
STATIC BOOLEAN Overlap(CONST VOID *P,UINTN N,CONST VOID *Q,UINTN M){return (UINTN)P<(UINTN)Q+M&&(UINTN)Q<(UINTN)P+N;}
STATIC BOOLEAN Identity(CR_ADRENO_CP_CONTEXT *C){
 if(!Span(C,sizeof(*C),8)||C->Self!=C||!Span(C->OriginalIo,sizeof(C->Io),8)||!Span(C->OriginalBacking,sizeof(C->Backing),8))return FALSE;
 CONST CR_ADRENO_CP_IO *I=C->OriginalIo;CONST CR_ADRENO_CP_BACKING *B=C->OriginalBacking;
 if(I->Context!=C->Io.Context||I->ContextBytes!=C->Io.ContextBytes||I->Incarnation!=C->Io.Incarnation||
 I->Check!=C->Io.Check||I->Read!=C->Io.Read||I->Write!=C->Io.Write||I->Sync!=C->Io.Sync||I->Secure!=C->Io.Secure||B->ChipId!=C->Backing.ChipId)return FALSE;
 for(UINT32 N=0;N<CrAdrenoCpBuffers;++N)if(B->Region[N].Cpu!=C->Backing.Region[N].Cpu||B->Region[N].Bytes!=C->Backing.Region[N].Bytes||B->Region[N].Iova!=C->Backing.Region[N].Iova)return FALSE;
 return TRUE;
}
STATIC BOOLEAN Output(CR_ADRENO_CP_CONTEXT *C,CONST VOID *P){
 if(!Span(P,sizeof(C->Outcome),8)||Overlap(P,sizeof(C->Outcome),C,sizeof(*C))||
 Overlap(P,sizeof(C->Outcome),C->OriginalIo,sizeof(C->Io))||Overlap(P,sizeof(C->Outcome),C->OriginalBacking,sizeof(C->Backing))||
 Overlap(P,sizeof(C->Outcome),C->Io.Context,C->Io.ContextBytes))return FALSE;
 for(UINT32 N=0;N<CrAdrenoCpBuffers;++N)if(Overlap(P,sizeof(C->Outcome),(CONST VOID *)C->Backing.Region[N].Cpu,C->Backing.Region[N].Bytes))return FALSE;
 return TRUE;
}
STATIC CR_STATUS Live(CR_ADRENO_CP_CONTEXT *C){if(!Identity(C))return CR_INVALID_PARAMETER;
 if(CrAtomicLoad32(&C->Gate)&CR_ADRENO_CP_CLOSED)return CR_ABORT;
 return C->Outcome.Faulted?C->Outcome.FirstFailure:CR_SUCCESS;}
STATIC CR_STATUS Fault(CR_ADRENO_CP_CONTEXT *C,CR_STATUS S){if(!CR_ERROR(S))S=CR_DEVICE_ERROR;
 C->Outcome.Faulted=1;if(C->Outcome.FirstFailure==CR_SUCCESS)C->Outcome.FirstFailure=S;return S;}
STATIC CR_STATUS Checked(CR_ADRENO_CP_CONTEXT *C){CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;
 S=C->Io.Check(C->Io.Context,C->Io.Incarnation);return S==CR_SUCCESS?Live(C):Fault(C,S);}
STATIC CR_STATUS Enter(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_OUTCOME *O,BOOLEAN Query){
 if(!Identity(C)||!Output(C,O))return CR_INVALID_PARAMETER;UINT32 G=CrAtomicLoad32(&C->Gate);
 if(G&ACTIVE)return CR_BUSY;if(!Query){CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;}
 return Cas(&C->Gate,G,G|ACTIVE)==G?CR_SUCCESS:CR_BUSY;
}
STATIC CR_STATUS Finish(CR_ADRENO_CP_CONTEXT *C,CR_STATUS S,CR_ADRENO_CP_OUTCOME *O){
 if(S!=CR_SUCCESS&&S!=CR_NOT_READY&&C->Outcome.RegisterWrites)S=Fault(C,S);
 if(S==CR_SUCCESS||S==CR_NOT_READY){CR_STATUS L=Live(C);if(L!=CR_SUCCESS)S=Fault(C,L);}
 C->Outcome.LastStatus=S;*O=C->Outcome;CrAtomicAnd32(&C->Gate,~ACTIVE);return S;
}
CR_STATUS CpRead(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_SPACE N,UINT32 R,UINT32 *V){CR_STATUS S=Checked(C);if(S!=CR_SUCCESS)return S;
 S=C->Io.Read(C->Io.Context,C->Io.Incarnation,N,R,V);return S==CR_SUCCESS?Checked(C):Fault(C,S);}
CR_STATUS CpWrite(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_SPACE N,UINT32 R,UINT32 V){CR_STATUS S=Checked(C);if(S!=CR_SUCCESS)return S;
 ++C->Outcome.RegisterWrites;S=C->Io.Write(C->Io.Context,C->Io.Incarnation,N,R,V);return S==CR_SUCCESS?Checked(C):Fault(C,S);}
CR_STATUS CpRmw(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_SPACE N,UINT32 R,UINT32 Clear,UINT32 Set){UINT32 V;CR_STATUS S=CpRead(C,N,R,&V);return S==CR_SUCCESS?CpWrite(C,N,R,(V&~Clear)|Set):S;}
CR_STATUS CpWrite64(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_SPACE N,UINT32 R,UINT64 V){CR_STATUS S=CpWrite(C,N,R,(UINT32)V);return S==CR_SUCCESS?CpWrite(C,N,R+1,(UINT32)(V>>32)):S;}
STATIC CR_STATUS Sync(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_BUFFER B,BOOLEAN Release){CR_STATUS S=Checked(C);if(S!=CR_SUCCESS)return S;
 S=C->Io.Sync(C->Io.Context,C->Io.Incarnation,B,Release);return S==CR_SUCCESS?Checked(C):Fault(C,S);}
STATIC CR_STATUS Secure(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_SECURE_STAGE Stage){CR_STATUS S=Checked(C);if(S!=CR_SUCCESS)return S;
 S=C->Io.Secure(C->Io.Context,C->Io.Incarnation,Stage);return S==CR_SUCCESS?Checked(C):Fault(C,S);}
STATIC UINT32 Odd(UINT32 N){N^=N>>16;N^=N>>8;N^=N>>4;return (~0x6996U>>(N&15))&1;}
STATIC UINT32 Packet(UINT32 Op,UINT32 Count){return 0x70000000U|(Odd(Op)<<23)|(Op<<16)|(Odd(Count)<<15)|Count;}
STATIC CR_STATUS Publish(CR_ADRENO_CP_CONTEXT *C,CONST UINT32 *Words,UINT32 Count,UINT32 Bit){
 if(!Count||Count>128||C->Outcome.Wptr>CR_ADRENO_CP_RING_BYTES/4-Count||C->Outcome.PublicationAttempts&Bit)return Fault(C,CR_INVALID_PARAMETER);
 CR_STATUS S=Checked(C);if(S!=CR_SUCCESS)return S;volatile UINT32 *R=C->Backing.Region[CrAdrenoCpRing].Cpu;
 UINT32 End=C->Outcome.Wptr+Count;for(UINT32 N=0;N<Count;++N)R[C->Outcome.Wptr+N]=Words[N];
 S=Sync(C,CrAdrenoCpRing,TRUE);if(S!=CR_SUCCESS)return S;
 /* Attempt journal precedes WPTR. A failed write or dropped-fence observation
    never rolls it back or permits replay. Keep all backing until real stop. */
 C->Outcome.PublicationAttempts|=Bit;C->Outcome.Wptr=End;
 S=CpWrite(C,CrAdrenoCpGpu,CP_RB_WPTR,End);if(S!=CR_SUCCESS)return S;
 C->Outcome.PublicationAccepted|=Bit;UINT32 Fence=0;
 S=CpRead(C,CrAdrenoCpGmu,GMU_AHB_FENCE_STATUS,&Fence);if(S!=CR_SUCCESS)return S;
 return Fence&1?Fault(C,CR_DEVICE_ERROR):CR_SUCCESS;
}
STATIC CR_STATUS Idle(CR_ADRENO_CP_CONTEXT *C){UINT32 R=0,S=0,G=0,F=0;
 CR_STATUS X=CpRead(C,CrAdrenoCpGpu,CP_RB_RPTR,&R);if(X!=CR_SUCCESS)return X;
 if(R>=CR_ADRENO_CP_RING_BYTES/4||R>C->Outcome.Wptr)return Fault(C,CR_COMPROMISED_DATA);
 X=CpRead(C,CrAdrenoCpGmu,GPU_GMU_AO_GPU_CX_BUSY_STATUS,&G);if(X!=CR_SUCCESS)return X;
 X=CpRead(C,CrAdrenoCpGpu,RBBM_STATUS,&S);if(X!=CR_SUCCESS)return X;
 X=CpRead(C,CrAdrenoCpGpu,RBBM_INT_0_STATUS,&F);if(X!=CR_SUCCESS)return X;
 if(F&(1U<<23))return Fault(C,CR_DEVICE_ERROR);
 return R==C->Outcome.Wptr&&!(S&0xfffffffeU)&&!(G&(1U<<23))?CR_SUCCESS:CR_NOT_READY;}
STATIC CR_STATUS Time(CR_ADRENO_CP_CONTEXT *C,UINT64 Now,BOOLEAN Deadline){if(Now<C->LastNowUs)return Fault(C,CR_INVALID_PARAMETER);C->LastNowUs=Now;
 return Deadline&&Now>=C->Outcome.DeadlineUs?Fault(C,CR_TIMEOUT):CR_SUCCESS;}
CR_STATUS CrAdrenoCpInit(CR_ADRENO_CP_CONTEXT *C,CONST CR_ADRENO_CP_IO *I,CONST CR_ADRENO_CP_BACKING *B){
 if(!Span(C,sizeof(*C),8)||!Span(I,sizeof(*I),8)||!Span(B,sizeof(*B),8)||!Span(I->Context,I->ContextBytes,1)||
 !I->Incarnation||!I->Check||!I->Read||!I->Write||!I->Sync||!I->Secure||B->ChipId!=0x07030001)return CR_INVALID_PARAMETER;
 CONST VOID *P[7]={C,I,B,I->Context};UINTN Z[7]={sizeof(*C),sizeof(*I),sizeof(*B),I->ContextBytes};
 for(UINT32 N=0;N<sizeof(*C);++N)if(((CONST UINT8 *)C)[N])return CR_INVALID_PARAMETER;
 for(UINT32 N=0;N<4;++N)for(UINT32 M=0;M<N;++M)if(Overlap(P[N],Z[N],P[M],Z[M]))return CR_INVALID_PARAMETER;
 CONST UINT32 Sizes[]={CR_ADRENO_CP_RING_BYTES,CR_ADRENO_CP_SHADOW_BYTES,CR_ADRENO_CP_POWER_BYTES,0};
 for(UINT32 N=0;N<CrAdrenoCpBuffers;++N){CONST CR_ADRENO_CP_REGION *R=&B->Region[N];
  if(!Span((CONST VOID *)R->Cpu,R->Bytes,4096)||!R->Iova||(R->Iova&4095)||R->Iova>=0x1000000000000ULL||R->Bytes>0x1000000000000ULL-R->Iova||
   (Sizes[N]&&R->Bytes!=Sizes[N])||(!Sizes[N]&&(R->Bytes<4||R->Bytes>0x100000||(R->Bytes&4095))))return CR_INVALID_PARAMETER;
  for(UINT32 M=0;M<4;++M)if(Overlap((CONST VOID *)R->Cpu,R->Bytes,P[M],Z[M]))return CR_INVALID_PARAMETER;
  for(UINT32 M=0;M<N;++M){CONST CR_ADRENO_CP_REGION *Q=&B->Region[M];if(Overlap((CONST VOID *)R->Cpu,R->Bytes,(CONST VOID *)Q->Cpu,Q->Bytes)||(R->Iova<Q->Iova+Q->Bytes&&Q->Iova<R->Iova+R->Bytes))return CR_INVALID_PARAMETER;}}
 C->Self=C;C->OriginalIo=I;C->OriginalBacking=B;C->Io=*I;C->Backing=*B;C->Outcome.Incarnation=I->Incarnation;C->Outcome.Phase=CrAdrenoCpNew;return CR_SUCCESS;
}
CR_STATUS CrAdrenoCpBegin(CR_ADRENO_CP_CONTEXT *C,UINT64 Now,UINT32 Timeout,CR_ADRENO_CP_OUTCOME *O){
 CR_STATUS S=Enter(C,O,FALSE);if(S!=CR_SUCCESS)return S;
 if(C->Outcome.Phase!=CrAdrenoCpNew||!Timeout||Timeout>1000000||Now>~(UINT64)0-Timeout)return Finish(C,CR_INVALID_PARAMETER,O);
 S=Secure(C,CrAdrenoCpPreflight);if(S!=CR_SUCCESS)return Finish(C,S,O);
 C->LastNowUs=Now;C->TimeoutUs=Timeout;C->Outcome.DeadlineUs=Now+Timeout;C->Outcome.Phase=CrAdrenoCpOobPending;
 C->Outcome.OobAttempted=1;S=CpWrite(C,CrAdrenoCpGmu,GMU_HOST2GMU_INTR_SET,1U<<30);
 return Finish(C,S==CR_SUCCESS?CR_NOT_READY:S,O);
}
STATIC CR_STATUS Configure(CR_ADRENO_CP_CONTEXT *C){
 CR_STATUS S;UINT32 G=~0U,V=~0U;
 /* A held pre-OS reset is not implicitly released by a successful HFI ACK.
    Actual GMU/domain startup must establish GX ON and GPU reset deasserted. */
 S=CpRead(C,CrAdrenoCpGmu,0x1f8d0,&G);if(S!=CR_SUCCESS)return S;
 if(G&3)return Fault(C,CR_NOT_READY);
 S=CpRead(C,CrAdrenoCpGpu,0x43,&V);if(S!=CR_SUCCESS)return S;
 if(V)return Fault(C,CR_NOT_READY);
 V=~0U;S=CpRead(C,CrAdrenoCpGpu,RBBM_INT_0_MASK,&V);if(S!=CR_SUCCESS)return S;
 if(V)return Fault(C,CR_BUSY); /* do not touch another IRQ owner */
 C->Outcome.InitAttempted=1;S=CpConfigureA730(C);if(S!=CR_SUCCESS)return S;
 C->Outcome.ApertureAttempted=1;S=Secure(C,CrAdrenoCpAperture);if(S!=CR_SUCCESS)return S;
 for(UINT32 N=0;N<CrAdrenoCpBuffers;++N){S=Sync(C,(CR_ADRENO_CP_BUFFER)N,TRUE);if(S!=CR_SUCCESS)return S;}
 S=CpWrite64(C,CrAdrenoCpGpu,CP_SQE_INSTR_BASE,C->Backing.Region[CrAdrenoCpSqe].Iova);if(S!=CR_SUCCESS)return S;
 S=CpWrite64(C,CrAdrenoCpGpu,CP_RB_BASE,C->Backing.Region[CrAdrenoCpRing].Iova);if(S!=CR_SUCCESS)return S;
 S=CpWrite(C,CrAdrenoCpGpu,CP_RB_CNTL,0x20c);if(S!=CR_SUCCESS)return S;
 S=CpWrite64(C,CrAdrenoCpGpu,CP_RB_RPTR_ADDR,C->Backing.Region[CrAdrenoCpShadow].Iova);if(S!=CR_SUCCESS)return S;
 S=CpWrite64(C,CrAdrenoCpGpu,CP_BV_RB_RPTR_ADDR,C->Backing.Region[CrAdrenoCpShadow].Iova+4);if(S!=CR_SUCCESS)return S;
 CONST UINT32 Registers[]={CP_SQE_INSTR_BASE,CP_SQE_INSTR_BASE+1,CP_RB_BASE,CP_RB_BASE+1,CP_RB_CNTL,
  CP_RB_RPTR_ADDR,CP_RB_RPTR_ADDR+1,CP_BV_RB_RPTR_ADDR,CP_BV_RB_RPTR_ADDR+1};
 UINT64 Sqe=C->Backing.Region[CrAdrenoCpSqe].Iova,Ring=C->Backing.Region[CrAdrenoCpRing].Iova,Shadow=C->Backing.Region[CrAdrenoCpShadow].Iova;
 UINT32 Expected[]={(UINT32)Sqe,(UINT32)(Sqe>>32),(UINT32)Ring,(UINT32)(Ring>>32),0x20c,
  (UINT32)Shadow,(UINT32)(Shadow>>32),(UINT32)(Shadow+4),(UINT32)((Shadow+4)>>32)};
 for(UINT32 N=0;N<sizeof(Registers)/sizeof(Registers[0]);++N){V=~Expected[N];S=CpRead(C,CrAdrenoCpGpu,Registers[N],&V);
  if(S!=CR_SUCCESS)return S;if(V!=Expected[N])return Fault(C,CR_DEVICE_ERROR);}
 C->Outcome.SqeAttempted=1;S=CpWrite(C,CrAdrenoCpGpu,CP_SQE_CNTL,1);if(S!=CR_SUCCESS)return S;
 V=0;S=CpRead(C,CrAdrenoCpGpu,CP_SQE_CNTL,&V);if(S!=CR_SUCCESS)return S;if(V!=1)return Fault(C,CR_DEVICE_ERROR);
 S=CpBuildPowerListA730(C);if(S!=CR_SUCCESS)return S;S=Sync(C,CrAdrenoCpPowerList,TRUE);if(S!=CR_SUCCESS)return S;
 UINT64 P=C->Backing.Region[CrAdrenoCpPowerList].Iova;
 UINT32 Init[]={Packet(0x17,1),1U<<27,Packet(0x48,7),0x14b,3,0x20000000,2,(UINT32)P,(UINT32)(P>>32),1U<<31};
 S=Publish(C,Init,10,1);if(S==CR_SUCCESS)C->Outcome.Phase=CrAdrenoCpInitPending;return S;
}
CR_STATUS CrAdrenoCpPoll(CR_ADRENO_CP_CONTEXT *C,UINT64 Now,CR_ADRENO_CP_OUTCOME *O){
 CR_STATUS S=Enter(C,O,FALSE);if(S!=CR_SUCCESS)return S;UINT32 P=C->Outcome.Phase;
 if(P!=CrAdrenoCpOobPending&&P!=CrAdrenoCpInitPending&&P!=CrAdrenoCpSecurePending&&P!=CrAdrenoCpProbePending)return Finish(C,CR_INVALID_PARAMETER,O);
 S=Time(C,Now,TRUE);if(S!=CR_SUCCESS)return Finish(C,S,O);
 if(P==CrAdrenoCpOobPending){UINT32 V=0;S=CpRead(C,CrAdrenoCpGmu,GMU_GMU2HOST_INTR_INFO,&V);
  if(S==CR_SUCCESS&&!(V&(1U<<31)))S=CR_NOT_READY;
  else if(S==CR_SUCCESS){C->Outcome.OobAcknowledged=1;S=CpWrite(C,CrAdrenoCpGmu,GMU_GMU2HOST_INTR_CLR,1U<<31);if(S==CR_SUCCESS)S=Configure(C);if(S==CR_SUCCESS)S=CR_NOT_READY;}}
 else if(P==CrAdrenoCpInitPending||P==CrAdrenoCpSecurePending){S=Idle(C);if(S==CR_SUCCESS&&P==CrAdrenoCpInitPending){
   C->Outcome.ZapAttempted=1;S=Secure(C,CrAdrenoCpZap);if(S==CR_SUCCESS){UINT32 W[]={Packet(0x66,1),0};S=Publish(C,W,2,2);}
   if(S==CR_SUCCESS){C->Outcome.Phase=CrAdrenoCpSecurePending;S=CR_NOT_READY;}}
  else if(S==CR_SUCCESS)C->Outcome.Phase=CrAdrenoCpReady;}
 else {S=Sync(C,CrAdrenoCpShadow,FALSE);if(S==CR_SUCCESS){volatile UINT32 *V=C->Backing.Region[CrAdrenoCpShadow].Cpu;
   C->Outcome.BvFence=V[2];C->Outcome.Fence=V[3];C->Outcome.Scratch=V[4];
   if(V[2]>1||V[3]>1||(V[3]&&(!V[2]||V[4]!=C->Outcome.Nonce)))S=Fault(C,CR_COMPROMISED_DATA);
   else if(V[2]==1&&V[3]==1&&V[4]==C->Outcome.Nonce){S=Idle(C);if(S==CR_SUCCESS)C->Outcome.Phase=CrAdrenoCpProbeComplete;}
   else S=CR_NOT_READY;}}
 return Finish(C,S,O);
}
CR_STATUS CrAdrenoCpSubmitProbe(CR_ADRENO_CP_CONTEXT *C,UINT32 Nonce,UINT64 Now,CR_ADRENO_CP_OUTCOME *O){
 CR_STATUS S=Enter(C,O,FALSE);if(S!=CR_SUCCESS)return S;
 if(!Nonce||C->Outcome.Phase!=CrAdrenoCpReady||Now>~(UINT64)0-C->TimeoutUs)return Finish(C,CR_INVALID_PARAMETER,O);
 S=Time(C,Now,FALSE);if(S!=CR_SUCCESS)return Finish(C,S,O);S=Sync(C,CrAdrenoCpShadow,FALSE);if(S!=CR_SUCCESS)return Finish(C,S,O);
 volatile UINT32 *V=C->Backing.Region[CrAdrenoCpShadow].Cpu;if(V[2]||V[3]||V[4])return Finish(C,Fault(C,CR_COMPROMISED_DATA),O);
 UINT64 B=C->Backing.Region[CrAdrenoCpShadow].Iova;
 UINT32 W[]={Packet(0x17,1),0x80000001U,Packet(0x17,1),3,Packet(0x65,1),0x101,Packet(0x17,1),1,Packet(0x3d,3),(UINT32)(B+16),(UINT32)((B+16)>>32),Nonce,
  Packet(0x46,1),0x18,Packet(0x46,1),0x19,Packet(0x17,1),2,Packet(0x46,4),0x08000031,(UINT32)(B+8),(UINT32)((B+8)>>32),1,
  Packet(0x17,1),1,Packet(0x14,4),0,(UINT32)(B+8),(UINT32)((B+8)>>32),1,
  Packet(0x46,4),0x08000031,(UINT32)(B+12),(UINT32)((B+12)>>32),1,Packet(0x17,1),3,Packet(0x65,1),0x100};
 C->Outcome.Nonce=Nonce;C->Outcome.Sequence=1;C->Outcome.DeadlineUs=Now+C->TimeoutUs;
 S=Publish(C,W,sizeof(W)/sizeof(W[0]),4);if(S==CR_SUCCESS){C->Outcome.Phase=CrAdrenoCpProbePending;S=CR_NOT_READY;}
 return Finish(C,S,O);
}
CR_STATUS CrAdrenoCpQuery(CR_ADRENO_CP_CONTEXT *C,CR_ADRENO_CP_OUTCOME *O){CR_STATUS S=Enter(C,O,TRUE);if(S!=CR_SUCCESS)return S;*O=C->Outcome;CrAtomicAnd32(&C->Gate,~ACTIVE);return CR_SUCCESS;}
CR_STATUS CrAdrenoCpSeal(CR_ADRENO_CP_CONTEXT *C){if(!Identity(C))return CR_INVALID_PARAMETER;CrAtomicOr32(&C->Gate,CR_ADRENO_CP_CLOSED);return CR_SUCCESS;}
CR_STATUS CrAdrenoCpDrain(CR_ADRENO_CP_CONTEXT *C){if(!Identity(C))return CR_INVALID_PARAMETER;UINT32 G=CrAtomicLoad32(&C->Gate);return !(G&CR_ADRENO_CP_CLOSED)?CR_NOT_READY:(G&ACTIVE)?CR_BUSY:CR_SUCCESS;}
