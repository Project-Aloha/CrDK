/* Actual retained CP engine; controlled register/DMA/provider endpoints. */
#include <Library/AdrenoCpCrLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned Checks;
#define CHECK(X) do{++Checks;if(!(X)){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,#X);abort();}}while(0)
#ifndef _KERNEL_MODE
#include <Library/SynchronizationLib.h>
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Old,UINT32 New){__atomic_compare_exchange_n(P,&Old,New,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Old;}
#endif
static CR_ADRENO_CP_CONTEXT C;static CR_ADRENO_CP_IO Io;static CR_ADRENO_CP_BACKING B;static CR_ADRENO_CP_OUTCOME O;
static _Alignas(4096) UINT32 Ring[8192],Shadow[1024],Power[2048],Sqe[19456];
static struct {UINT32 Reg[2][0x30000];unsigned Calls,Fail,Warn,Seal,Writes,Syncs,Secure[4],Consumed,Thread;BOOLEAN Dropped;} M;
static CR_STATUS Hit(void){++M.Calls;if(M.Seal==M.Calls)CHECK(CrAdrenoCpSeal(&C)==CR_SUCCESS);
 return M.Fail==M.Calls?CR_DEVICE_ERROR:M.Warn==M.Calls?(CR_STATUS)1:CR_SUCCESS;}
static CR_STATUS Check(void *P,UINT64 I){CHECK(P==&M&&I==9);return Hit();}
static CR_STATUS Read(void *P,UINT64 I,CR_ADRENO_CP_SPACE N,UINT32 R,UINT32 *V){CHECK(P==&M&&I==9&&N>=1&&N<=2&&R<0x30000);*V=M.Reg[N-1][R];return Hit();}
static CR_STATUS Write(void *P,UINT64 I,CR_ADRENO_CP_SPACE N,UINT32 R,UINT32 V){CHECK(P==&M&&I==9&&N>=1&&N<=2&&R<0x30000);++M.Writes;M.Reg[N-1][R]=V;
 if(N==2&&R==0x1f994&&V==(1U<<30))M.Reg[1][0x1f992]|=1U<<31;
 if(N==2&&R==0x1f991)M.Reg[1][0x1f992]&=~V;
 if(N==1&&R==0x807&&M.Dropped)M.Reg[1][0x23b13]|=1;
 return Hit();}
static CR_STATUS Sync(void *P,UINT64 I,CR_ADRENO_CP_BUFFER R,BOOLEAN To){CHECK(P==&M&&I==9&&(UINT32)R<4);(void)To;++M.Syncs;return Hit();}
static CR_STATUS Secure(void *P,UINT64 I,CR_ADRENO_CP_SECURE_STAGE S){CHECK(P==&M&&I==9&&S>=1&&S<=3);++M.Secure[S];return Hit();}
static void Init(void){memset(&C,0,sizeof(C));memset(&M,0,sizeof(M));memset(Ring,0,sizeof(Ring));memset(Shadow,0,sizeof(Shadow));memset(Power,0,sizeof(Power));memset(Sqe,0x33,sizeof(Sqe));memset(&O,0,sizeof(O));
 Io=(CR_ADRENO_CP_IO){&M,sizeof(M),9,Check,Read,Write,Sync,Secure};B=(CR_ADRENO_CP_BACKING){0x07030001,{{Ring,0x100000000ULL,sizeof(Ring)},{Shadow,0x100008000ULL,sizeof(Shadow)},{Power,0x100009000ULL,sizeof(Power)},{Sqe,0x100020000ULL,sizeof(Sqe)}}};
 CHECK(CrAdrenoCpInit(&C,&Io,&B)==CR_SUCCESS);CHECK(M.Calls==0);}
static UINT32 *Dma(UINT64 A){CHECK(A>=B.Region[1].Iova&&A<B.Region[1].Iova+B.Region[1].Bytes&&!(A&3));return &Shadow[(A-B.Region[1].Iova)/4];}
static void Consume(void){UINT32 End=M.Reg[0][0x807];CHECK(End<=8192);
 while(M.Consumed<End){UINT32 H=Ring[M.Consumed++],N=H&0x3fff,Op=(H>>16)&0x7f;CHECK((H>>28)==7&&N&&N<128&&N<=End-M.Consumed);UINT32 *W=Ring+M.Consumed;
  if(Op==0x17){CHECK(N==1);M.Thread=W[0]&3;}
  else if(Op==0x48){CHECK(N==7&&W[0]==0x14b&&W[1]==3&&W[2]==0x20000000&&W[3]==2&&W[6]==0x80000000);CHECK(((UINT64)W[4]|((UINT64)W[5]<<32))==B.Region[2].Iova);}
  else if(Op==0x66){CHECK(N==1&&!W[0]&&M.Secure[3]);}
  else if(Op==0x3d){CHECK(N==3&&M.Thread==1);*Dma(W[0]|((UINT64)W[1]<<32))=W[2];}
  else if(Op==0x46){if(N==1)CHECK(W[0]==0x18||W[0]==0x19);else {CHECK(N==4&&W[0]==0x08000031&&(M.Thread==1||M.Thread==2));*Dma(W[1]|((UINT64)W[2]<<32))=W[3];}}
  else if(Op==0x14){CHECK(N==4&&M.Thread==1&&!W[0]);CHECK(*Dma(W[1]|((UINT64)W[2]<<32))==W[3]);}
  else if(Op==0x65){CHECK(N==1&&(W[0]==0x100||W[0]==0x101));}else CHECK(FALSE);
  M.Consumed+=N;}
 M.Reg[0][0x806]=End;Shadow[0]=End;}
static CR_STATUS Progress(void){CR_STATUS S=CrAdrenoCpBegin(&C,0,1000,&O);if(S!=CR_NOT_READY||O.Faulted)return S;
 S=CrAdrenoCpPoll(&C,1,&O);if(S!=CR_NOT_READY||O.Faulted)return S;Consume();
 S=CrAdrenoCpPoll(&C,2,&O);if(S!=CR_NOT_READY||O.Faulted)return S;Consume();
 S=CrAdrenoCpPoll(&C,3,&O);if(S!=CR_SUCCESS)return S;
 S=CrAdrenoCpSubmitProbe(&C,0x12345678,4,&O);if(S!=CR_NOT_READY||O.Faulted)return S;Consume();return CrAdrenoCpPoll(&C,5,&O);}
static void Nominal(void){Init();CHECK(Progress()==CR_SUCCESS);CHECK(O.Phase==CrAdrenoCpProbeComplete&&O.Fence==1&&O.BvFence==1&&O.Scratch==0x12345678);
 CHECK(O.PublicationAttempts==7&&O.PublicationAccepted==7&&M.Secure[1]==1&&M.Secure[2]==1&&M.Secure[3]==1);
 unsigned W=M.Writes;CHECK(CrAdrenoCpSubmitProbe(&C,2,6,&O)!=CR_SUCCESS&&M.Writes==W);CHECK(CrAdrenoCpSeal(&C)==CR_SUCCESS&&CrAdrenoCpDrain(&C)==CR_SUCCESS);}
static void Faults(void){Init();CHECK(Progress()==CR_SUCCESS);unsigned Calls=M.Calls;
 for(unsigned Kind=0;Kind<3;++Kind)for(unsigned N=1;N<=Calls;++N){Init();if(Kind==0)M.Fail=N;else if(Kind==1)M.Warn=N;else M.Seal=N;
  CHECK(Progress()!=CR_SUCCESS);unsigned W=M.Writes;CHECK(CrAdrenoCpPoll(&C,9,&O)!=CR_SUCCESS&&M.Writes==W);CHECK(CrAdrenoCpSeal(&C)==CR_SUCCESS&&CrAdrenoCpDrain(&C)==CR_SUCCESS);}
 printf("callback positions %u x3\n",Calls);}
static void Guards(void){Init();Io.Incarnation++;CHECK(CrAdrenoCpBegin(&C,0,1000,&O)==CR_INVALID_PARAMETER&&M.Writes==0);
 Init();M.Reg[0][0x38]=1;CHECK(CrAdrenoCpBegin(&C,0,1000,&O)==CR_NOT_READY);CHECK(CrAdrenoCpPoll(&C,1,&O)!=CR_SUCCESS&&!O.InitAttempted&&M.Reg[0][0x38]==1);
 Init();M.Dropped=TRUE;CHECK(CrAdrenoCpBegin(&C,0,1000,&O)==CR_NOT_READY);CHECK(CrAdrenoCpPoll(&C,1,&O)!=CR_SUCCESS&&O.PublicationAccepted==1&&O.Faulted);
 Init();CHECK(CrAdrenoCpBegin(&C,0,10,&O)==CR_NOT_READY);CHECK(CrAdrenoCpPoll(&C,10,&O)==CR_TIMEOUT&&O.OobAttempted);
 Init();CHECK(CrAdrenoCpBegin(&C,0,1000,&O)==CR_NOT_READY);CHECK(CrAdrenoCpPoll(&C,1,&O)==CR_NOT_READY);M.Reg[0][0x806]=8192;CHECK(CrAdrenoCpPoll(&C,2,&O)==CR_COMPROMISED_DATA);
 Init();CHECK(CrAdrenoCpBegin(&C,0,1000,&O)==CR_NOT_READY);CHECK(CrAdrenoCpPoll(&C,1,&O)==CR_NOT_READY);Consume();M.Reg[0][0x201]=1U<<23;CHECK(CrAdrenoCpPoll(&C,2,&O)==CR_DEVICE_ERROR);
 Init();CHECK(CrAdrenoCpBegin(&C,0,1000,&O)==CR_NOT_READY);CHECK(CrAdrenoCpPoll(&C,1,&O)==CR_NOT_READY);Consume();M.Reg[1][0x23b0c]=1U<<23;CHECK(CrAdrenoCpPoll(&C,2,&O)==CR_NOT_READY);CHECK(M.Secure[3]==0);
}
int main(void){Nominal();Faults();Guards();printf("CP90 portable PASS %u checks\n",Checks);return 0;}
