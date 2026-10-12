/* Architected non-secure SMMUv2 retirement, source Linux arm-smmu + IHI0062D.
 * SPDX-License-Identifier: GPL-2.0-only
 * No qcom_smmu's last-CB bypass emulation, no SCR0/global TLB writes. */
#include <Library/AdrenoBootCrLib.h>
#define BIT(N) (1U<<(N))
#define TRY(X) do{CR_STATUS S_=(X);if(S_!=CR_SUCCESS)return S_;}while(0)
static BOOLEAN Same(CONST VOID *A,CONST VOID *B,UINTN N){CONST UINT8 *P=A,*Q=B;for(UINTN I=0;I<N;++I)if(P[I]!=Q[I])return FALSE;return TRUE;}
static CR_STATUS Final(CR_STATUS S){return S==CR_SUCCESS?S:CR_ERROR(S)?S:CR_DEVICE_ERROR;}
static BOOLEAN Span(CONST VOID *P,UINTN N,UINTN A){return P&&N&&!((UINTN)P&(A-1))&&(UINTN)P<=~(UINTN)0-N;}
static BOOLEAN Overlap(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn){return (UINTN)A<(UINTN)B+Bn&&(UINTN)B<(UINTN)A+An;}
static BOOLEAN Identity(CR_ADRENO_BOOT_CONTEXT *C){return Span(C,sizeof(*C),8)&&C->Self==C&&Span(C->OriginalIo,sizeof(*C->OriginalIo),8)&&
 C->Stop&&C->Incarnation&&C->Io.Incarnation==C->Incarnation&&Same(&C->Io,C->OriginalIo,sizeof(C->Io));}
static CR_STATUS Access(CR_ADRENO_BOOT_CONTEXT *C){if(!Identity(C))return CR_INVALID_PARAMETER;
 if(C->Closed||C->Uncertain)return CR_NOT_READY;return Final(C->Io.Check(C->Io.Context,C->Incarnation));}
static CR_STATUS Read(CR_ADRENO_BOOT_CONTEXT *C,UINT32 At,UINT32 *Out){TRY(Access(C));
 if((At&3)||At>CR_ADRENO_ROUTE_SMMU_BYTES-4)return CR_INVALID_PARAMETER;
 UINT32 V=0xdeadbeefU;TRY(Final(C->Io.Read(C->Io.Context,C->Incarnation,At,&V)));TRY(Access(C));*Out=V;return CR_SUCCESS;}
static CR_STATUS Write(CR_ADRENO_BOOT_CONTEXT *C,UINT32 At,UINT32 Value){TRY(Access(C));
 if((At&3)||At>CR_ADRENO_ROUTE_SMMU_BYTES-4)return CR_INVALID_PARAMETER;C->Attempted=TRUE;
 TRY(Final(C->Io.Write(C->Io.Context,C->Incarnation,At,Value)));TRY(Access(C));return Final(C->Io.Barrier(C->Io.Context,C->Incarnation));}
static CR_STATUS Equal(CR_ADRENO_BOOT_CONTEXT *C,UINT32 At,UINT32 Value){UINT32 V;TRY(Read(C,At,&V));return V==Value?CR_SUCCESS:CR_DEVICE_ERROR;}

static CR_STATUS Inventory(CR_ADRENO_BOOT_CONTEXT *C,CR_ADRENO_BOOT_INVENTORY *I,BOOLEAN Before){
 *I=(CR_ADRENO_BOOT_INVENTORY){0};TRY(Read(C,0x20,&I->Id[0]));TRY(Read(C,0x24,&I->Id[1]));TRY(Read(C,0x28,&I->Id[2]));TRY(Read(C,0,&I->Control));
 I->StreamCount=I->Id[0]&255;I->ContextCount=I->Id[1]&255;
 if((I->Id[0]&0x48000000U)!=0x48000000U||(I->Id[1]>>31)||!(I->Id[2]&0x1000)||
 (I->Control&9)||!(I->Control&0x400)||!I->StreamCount||I->StreamCount>24||
 !I->ContextCount||I->ContextCount>32||(I->Id[1]&0xff0000))return CR_UNSUPPORTED;
 I->ContextBase=(2U<<((I->Id[1]>>28)&7))*4096U;
 if(I->ContextBase<0x2000||I->ContextBase>0x40000||I->ContextCount>(0x40000-I->ContextBase)/4096)return CR_UNSUPPORTED;
 for(UINT32 N=0;N<I->StreamCount;++N){TRY(Read(C,0x800+4*N,&I->Smr[N]));TRY(Read(C,0xc00+4*N,&I->S2cr[N]));}
 for(UINT32 N=0;N<I->ContextCount;++N){TRY(Read(C,0x1000+4*N,&I->Cbar[N]));TRY(Read(C,0x1800+4*N,&I->Cba2r[N]));
  TRY(Read(C,I->ContextBase+4096*N,&I->Sctlr[N]));TRY(Read(C,I->ContextBase+4096*N+0x24,&I->TtbrAsid[N]));}
 for(UINT32 N=0;N<I->StreamCount;++N){UINT32 Smr=I->Smr[N],S2=I->S2cr[N];
  if(!(Smr&0x80000000U))continue;
  UINT32 Mask=(Smr>>16)&0x7fff,Id=Smr&0x7fff,Type=(S2>>16)&3,Cb=S2&255,Aliases=0;
  if(Smr&0x8000)return CR_UNSUPPORTED;
  /* Test every qualified alias against the full mask first. A broad foreign
   * wildcard overlapping even one own stream is not an ignorable fault slot. */
  static CONST UINT32 Sids[6]={0,1,0x400,0x401,5,0x405};
  for(UINT32 A=0;A<6;++A)if(((Sids[A]^Id)&~Mask)==0)Aliases|=BIT(A);
  if(!Aliases){if(Type!=2)return CR_UNSUPPORTED;continue;}
  if((Mask!=0&&Mask!=0x400)||(Id&Mask)||(I->AliasMask&Aliases))return CR_UNSUPPORTED;
  I->AliasMask|=Aliases;I->SlotMask|=BIT(N);
  if(Type==0){if(!Before||Cb>=I->ContextCount||(S2&0x3fc00U)||
      ((I->Cbar[Cb]>>16)&3)!=1||!(I->Cba2r[Cb]&1))return CR_UNSUPPORTED;
    I->ContextMask|=BIT(Cb);
  }else if(Type!=2)return CR_UNSUPPORTED;
 }
 if(I->AliasMask!=0x3f)return CR_UNSUPPORTED;
 for(UINT32 N=0;N<I->ContextCount;++N)if(I->Sctlr[N]&1){
  if(!(I->ContextMask&BIT(N)))return CR_UNSUPPORTED;}
 /* Complete fresh global state and slot scan; a changed producer cannot be
  * hidden between one captured SMR and later stop. */
 for(UINT32 N=0;N<I->StreamCount;++N){TRY(Equal(C,0x800+4*N,I->Smr[N]));TRY(Equal(C,0xc00+4*N,I->S2cr[N]));}
 TRY(Equal(C,0,I->Control));return CR_SUCCESS;
}
static CR_STATUS Tlbi(CR_ADRENO_BOOT_CONTEXT *C,UINT32 N){UINT32 B=C->Before.ContextBase+4096*N;
 /* Linux S1 full invalidation uses this context's TTBR ASID, then context sync.
  * No hardware translation tables are read/freed; inherited pages stay reserved. */
 TRY(Write(C,B+0x610,C->Before.TtbrAsid[N]>>16));TRY(Write(C,B+0x7f0,0xffffffffU));
 for(UINT32 Us=0;Us<=10000;Us+=10){UINT32 V;TRY(Read(C,B+0x7f4,&V));if(!(V&1))return CR_SUCCESS;
  if(Us==10000)break;TRY(Final(C->Io.DelayUs(C->Io.Context,C->Incarnation,10)));}
 return CR_TIMEOUT;
}
static CR_STATUS Verify(CR_ADRENO_BOOT_CONTEXT *C){
 TRY(CrAdrenoStopCheck(C->Stop,&C->StopEvidence));TRY(Inventory(C,&C->After,FALSE));
 CONST CR_ADRENO_BOOT_INVENTORY *B=&C->Before,*A=&C->After;
 if(!Same(B->Id,A->Id,sizeof(B->Id))||A->Control!=B->Control||A->StreamCount!=B->StreamCount||
 A->ContextCount!=B->ContextCount||A->ContextBase!=B->ContextBase||A->SlotMask!=B->SlotMask)return CR_DEVICE_ERROR;
 for(UINT32 N=0;N<B->StreamCount;++N){UINT32 Expected=(B->SlotMask&BIT(N))?((B->S2cr[N]&~0x30000U)|0x20000U):B->S2cr[N];
  if(A->Smr[N]!=B->Smr[N]||A->S2cr[N]!=Expected)return CR_DEVICE_ERROR;}
 for(UINT32 N=0;N<B->ContextCount;++N){UINT32 Expected=(B->ContextMask&BIT(N))?0:B->Sctlr[N];
  if(A->Cbar[N]!=B->Cbar[N]||A->Cba2r[N]!=B->Cba2r[N]||A->Sctlr[N]!=Expected||A->TtbrAsid[N]!=B->TtbrAsid[N])return CR_DEVICE_ERROR;}
 return Access(C);
}
CR_STATUS CrAdrenoBootInit(CR_ADRENO_BOOT_CONTEXT *C,CONST CR_ADRENO_BOOT_IO *Io,CR_ADRENO_STOP_CONTEXT *Stop){
 if(!Span(C,sizeof(*C),8)||!Span(Io,sizeof(*Io),8)||!Span(Stop,sizeof(*Stop),8)||
 Overlap(C,sizeof(*C),Io,sizeof(*Io))||Overlap(C,sizeof(*C),Stop,sizeof(*Stop))||
 Overlap(Io,sizeof(*Io),Stop,sizeof(*Stop)))return CR_INVALID_PARAMETER;
 if(C->Self)return CR_ALREADY_STARTED;
 if(!Span(Io->Context,Io->ContextBytes,1)||!Io->Incarnation||!Io->Check||!Io->Read||!Io->Write||!Io->Barrier||!Io->DelayUs||
 Overlap(C,sizeof(*C),Io->Context,Io->ContextBytes)||Overlap(Io,sizeof(*Io),Io->Context,Io->ContextBytes)||
 Overlap(Stop,sizeof(*Stop),Io->Context,Io->ContextBytes)||Stop->Self!=Stop||Stop->StartIncarnation!=Io->Incarnation)return CR_INVALID_PARAMETER;
 CR_ADRENO_BOOT_CONTEXT New={0};New.Self=C;New.OriginalIo=Io;New.Io=*Io;New.Stop=Stop;New.Incarnation=Io->Incarnation;*C=New;return CR_SUCCESS;
}
static CR_STATUS Retire(CR_ADRENO_BOOT_CONTEXT *C){
 C->Stage=1;TRY(Inventory(C,&C->Before,TRUE));C->Stage=2;
 /* Mark before first Stop callback: a failed stop cannot be repeated by this
  * transaction and cannot return an apparently untouched route owner. */
 C->Attempted=TRUE;TRY(CrAdrenoStopFence(C->Stop));TRY(CrAdrenoStopCheck(C->Stop,&C->StopEvidence));
 CR_ADRENO_BOOT_INVENTORY Fresh;TRY(Inventory(C,&Fresh,TRUE));
 if(!Same(&Fresh,&C->Before,sizeof(Fresh)))return CR_DEVICE_ERROR;
 C->Stage=3;for(UINT32 N=0;N<C->Before.StreamCount;++N)if(C->Before.SlotMask&BIT(N)){
  UINT32 V=(C->Before.S2cr[N]&~0x30000U)|0x20000U;C->FaultAttempted|=BIT(N);
  TRY(Write(C,0xc00+4*N,V));TRY(Equal(C,0xc00+4*N,V));C->FaultDone|=BIT(N);}
 C->Stage=4;for(UINT32 N=0;N<C->Before.ContextCount;++N)if(C->Before.ContextMask&BIT(N)){
  TRY(Tlbi(C,N));C->DisableAttempted|=BIT(N);TRY(Write(C,C->Before.ContextBase+4096*N,0));
  TRY(Equal(C,C->Before.ContextBase+4096*N,0));C->DisableDone|=BIT(N);TRY(Tlbi(C,N));C->TlbiDone|=BIT(N);}
 C->Stage=5;TRY(Verify(C));C->Retired=TRUE;C->Stage=6;return CR_SUCCESS;
}
CR_STATUS CrAdrenoBootRetire(CR_ADRENO_BOOT_CONTEXT *C){
 CR_STATUS S=Access(C);if(S!=CR_SUCCESS)return S;if(C->Stage||C->Retired)return CR_ALREADY_STARTED;
 S=Retire(C);C->LastStatus=S;if(S!=CR_SUCCESS){if(C->Attempted)C->Uncertain=TRUE;C->Closed=TRUE;}return S;
}
CR_STATUS CrAdrenoBootRecord(CR_ADRENO_BOOT_CONTEXT *C,CONST CR_ADRENO_ROUTE_FACTS *F,UINT64 Pa,CR_ADRENO_HANDOFF_RECORD *Out){
 CR_STATUS S=Access(C);if(S!=CR_SUCCESS)return S;if(!C->Retired||!Span(Out,sizeof(*Out),8)||!Span(F,sizeof(*F),8)||
 Overlap(Out,sizeof(*Out),C,sizeof(*C))||Overlap(Out,sizeof(*Out),C->Stop,sizeof(*C->Stop))||
 Overlap(Out,sizeof(*Out),C->OriginalIo,sizeof(*C->OriginalIo))||Overlap(Out,sizeof(*Out),C->Io.Context,C->Io.ContextBytes)||
 Overlap(Out,sizeof(*Out),F,sizeof(*F))||CrAdrenoRouteValidateFacts(F)!=CR_SUCCESS||Pa<0x100000||(Pa&4095)||Pa>0x7ffffffffffff000ULL)return CR_INVALID_PARAMETER;
 S=Verify(C);if(S!=CR_SUCCESS){C->Uncertain=C->Closed=TRUE;return S;}
 CR_ADRENO_HANDOFF_RECORD R={0};R.Signature=CR_ADRENO_RECORD_SIGNATURE;R.Version=1;R.Size=sizeof(R);R.Flags=3;R.ChipId=0x8450;
 R.Incarnation=C->Incarnation;R.RecordPhysical=Pa;R.Route=*F;R.Stop=C->StopEvidence;
 for(UINT32 N=0;N<3;++N)R.SmmuId[N]=C->Before.Id[N];R.SmmuControl=C->Before.Control;
 R.StreamCount=C->Before.StreamCount;R.ContextCount=C->Before.ContextCount;R.RetiredStreams=C->Before.AliasMask;R.RetiredContexts=C->Before.ContextMask;
 R.BeforeInventoryCrc=CrAdrenoRouteCrc32(&C->Before,sizeof(C->Before));R.AfterInventoryCrc=CrAdrenoRouteCrc32(&C->After,sizeof(C->After));
 /* Sole serialized native owner closes admission before record escapes. Real
  * provider must also pin its protected clock/reset writers through OS entry. */
 S=CrAdrenoStopSeal(C->Stop);if(S!=CR_SUCCESS){C->Uncertain=C->Closed=TRUE;return Final(S);}C->Closed=TRUE;
 R.Crc32=CrAdrenoRouteCrc32(&R,sizeof(R));*Out=R;return CR_SUCCESS;
}
