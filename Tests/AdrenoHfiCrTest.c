/* Actual retained HFI transport + independent original Linux wire oracle. GPL-2.0-only */
#include <Library/AdrenoHfiCrLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
static _Atomic unsigned Checks;
static unsigned Groups;
#define CHECK(X) do{++Checks;if(!(X)){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,#X);abort();}}while(0)
#ifndef _KERNEL_MODE
#include <Library/SynchronizationLib.h>
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Old,UINT32 New){
  __atomic_compare_exchange_n(P,&Old,New,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Old;
}
#endif
void OracleQueues(void *,UINT32);
UINT32 OracleEmptyRequest(void);
void OracleBoot(const CR_ADRENO_HFI_BOOT_PLAN *,UINT32,UINT32 *,UINT32 *,UINT32 *);
static _Alignas(4096) UINT32 Memory[4096],Expected[4096];
static CR_ADRENO_HFI_CONTEXT C;
static CR_ADRENO_HFI_IO Io;
static CR_ADRENO_HFI_BACKING Backing;
static CR_ADRENO_HFI_BOOT_PLAN Plan;
static CR_ADRENO_HFI_COMMAND Cmd;
static CR_ADRENO_HFI_OUTCOME Out;
typedef struct {UINT32 Calls,Fail,Warn,Seal,Pause,Skip,Double,Kicks,Depth,Released,LastWrite,Race;
  BOOLEAN Denied;CR_STATUS SealStatus;} MODEL;
static MODEL M;
static pthread_mutex_t Lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t Cond=PTHREAD_COND_INITIALIZER;
static int Paused,Resume;
static CR_STATUS ThreadStatus;
static void Ack(UINT32 Returned,UINT32 Error);
static CR_STATUS Hit(void){
  ++M.Calls;
  if(M.Calls==M.Pause){pthread_mutex_lock(&Lock);Paused=1;pthread_cond_broadcast(&Cond);
    while(!Resume)pthread_cond_wait(&Cond,&Lock);pthread_mutex_unlock(&Lock);}
  if(M.Calls==M.Seal)M.SealStatus=CrAdrenoHfiSeal(&C);
  if(M.Calls==M.Fail)return CR_DEVICE_ERROR;
  if(M.Calls==M.Warn)return (CR_STATUS)1;
  return CR_SUCCESS;
}
static CR_STATUS Execute(void *P,UINT64 Epoch,CR_ADRENO_HFI_ACCESS A,CR_ADRENO_HFI_OPERATION F,void *Arg){
  CHECK(P==&M && Epoch==7 && A>=1 && A<=3 && !M.Depth);
  CR_STATUS S=Hit();if(S!=CR_SUCCESS)return S;if(M.Denied)return CR_NOT_READY;
  if(M.Skip)return CR_SUCCESS;
  ++M.Depth;S=F(Arg);if(M.Double)(void)F(Arg);--M.Depth;
  CR_STATUS After=Hit();return After==CR_SUCCESS?S:After;
}
static CR_STATUS Barrier(void *P,UINT64 Epoch,CR_ADRENO_HFI_ORDER O){
  CHECK(P==&M && Epoch==7 && M.Depth==1 && (O==1||O==2));
  CR_STATUS S=Hit();if(S!=CR_SUCCESS)return S;
  if(O==CrAdrenoHfiAcquire && M.Race && Memory[26]==1){M.Race=0;Ack(C.Outcome.Header,0);}
  atomic_thread_fence(memory_order_seq_cst);if(O==CrAdrenoHfiRelease)++M.Released;return CR_SUCCESS;
}
static CR_STATUS Kick(void *P,UINT64 Epoch){
  CHECK(P==&M && Epoch==7 && M.Depth==1);++M.Kicks;
  CHECK(M.Released>=2 && Memory[17]==C.TxIndex && (C.Outcome.Effects&CR_ADRENO_HFI_TX_PUBLISHED));
  return Hit();
}
static void Reset(BOOLEAN Prepare){
  memset(&C,0,sizeof(C));memset(&M,0,sizeof(M));memset(Memory,0x71,sizeof(Memory));
  memset(&Out,0,sizeof(Out));
  Io=(CR_ADRENO_HFI_IO){&M,sizeof(M),7,Execute,Barrier,Kick};
  Backing=(CR_ADRENO_HFI_BACKING){Memory,sizeof(Memory),0x60001000};
  CHECK(CrAdrenoHfiInit(&C,&Io,&Backing,7)==CR_SUCCESS);CHECK(M.Calls==0);
  if(Prepare)CHECK(CrAdrenoHfiPrepareQueues(&C)==CR_SUCCESS);
  M.Calls=0;M.Released=0;
}
static void Build(void){
  CR_RPMH_ARC_RESOURCE Rails[4]={0};const char *Names[]={"gfx.lvl","cx.lvl","mx.lvl","gmxc.lvl"};
  for(unsigned N=0;N<4;++N){strcpy(Rails[N].Name,Names[N]);Rails[N].Address=0x30000+N*16;
    Rails[N].Kind=CrRpmhArcPower;Rails[N].LevelCount=26;
    for(unsigned J=1;J<26;++J)Rails[N].Levels[J]=J*16;}
  memset(&Plan,0,sizeof(Plan));Plan.Version=1;Plan.Size=sizeof(Plan);Plan.ChipId=0x07030001;
  CHECK(CrAdrenoPowerBuild(CrAdrenoSm8450PowerProfile(),&Rails[0],&Rails[1],&Rails[2],&Rails[3],&Plan.Power)==CR_SUCCESS);
  for(unsigned N=0;N<4;++N)Plan.BcmAddress[N]=0x50000+N*16;
  CHECK(CrAdrenoHfiValidateBoot(&Plan)==CR_SUCCESS);
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,0,&Cmd)==CR_SUCCESS);
}
static void Push(unsigned Q,UINT32 Header,UINT32 Returned,UINT32 Error){
  UINT32 *H=&Memory[6+12*Q],*R=&Memory[1024*(Q+1)],At=H[11],N=(Header>>8)&255,Pad=(N+3)&~3U;
  CHECK(Pad<1024 && ((H[10]-At-1)&1023)>=Pad);
  for(unsigned I=0;I<Pad;++I)R[(At+I)&1023]=I==0?Header:I==1?Returned:I==2?Error:0;
  H[11]=(At+Pad)&1023;
}
static void Ack(UINT32 Returned,UINT32 Error){Push(1,126|(3<<8)|(1<<16),Returned,Error);}
static void FirmwareConsume(void){Memory[16]=Memory[17];}
static void QueryFault(void){
  CHECK(CrAdrenoHfiQuery(&C,&Out)==CR_SUCCESS);CHECK(Out.Faulted);
  UINT32 Calls=M.Calls;CR_ADRENO_HFI_OUTCOME Hold=Out;
  CHECK(CrAdrenoHfiPoll(&C,1,8,&Out)!=CR_SUCCESS);CHECK(M.Calls==Calls);
  CHECK(CrAdrenoHfiPrepareQueues(&C)!=CR_SUCCESS);CHECK(M.Calls==Calls);
  CHECK(CrAdrenoHfiQuery(&C,&Out)==CR_SUCCESS);CHECK(Out.Effects==Hold.Effects);
  CHECK(CrAdrenoHfiSeal(&C)==CR_SUCCESS);CHECK(CrAdrenoHfiDrain(&C)==CR_SUCCESS);CHECK(M.Calls==Calls);
}
static void Oracle(void){
  ++Groups;Build();Reset(FALSE);memset(Expected,0,sizeof(Expected));OracleQueues(Expected,Backing.GmuIova);
  CHECK(CrAdrenoHfiPrepareQueues(&C)==CR_SUCCESS);CHECK(!memcmp(Expected,Memory,sizeof(Memory)));
  for(unsigned I=0;I<4;++I){UINT32 Words[160]={0},Count=0,Id=0;
    OracleBoot(&Plan,I,Words,&Count,&Id);CHECK(CrAdrenoHfiBuildBootCommand(&Plan,I,&Cmd)==CR_SUCCESS);
    CHECK(Cmd.Id==Id && Cmd.Count==Count);CHECK(Words[0]==((1U<<20)|(Count<<8)|Id));
    Words[0]=0;CHECK(!memcmp(Words,Cmd.Words,sizeof(Words)));
  }
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,0,&Cmd)==CR_SUCCESS);
}
static void Boot(void){
  ++Groups;Reset(TRUE);CHECK(CrAdrenoHfiBootBegin(&C,&Plan,0,100,&Out)==CR_NOT_READY);
  for(unsigned I=0;I<4;++I){
    UINT32 ExpectedWords[160]={0},Count,Id;OracleBoot(&Plan,I,ExpectedWords,&Count,&Id);
    UINT32 Start=(Memory[17]-((Count+3)&~3U))&1023;ExpectedWords[0]=((I+1)<<20)|(Count<<8)|Id;
    for(unsigned N=0;N<Count;++N)CHECK(Memory[1024+((Start+N)&1023)]==ExpectedWords[N]);
    CHECK(Out.BootIndex==I && !Out.BootComplete && Out.Outstanding && !Out.Acknowledged);
    CHECK(CrAdrenoHfiSubmit(&C,&Cmd,I,100,&Out)==CR_BUSY);
    FirmwareConsume();Ack(C.Outcome.Header,0);
    CHECK(CrAdrenoHfiBootPoll(&C,I+1,8,&Out)==(I==3?CR_SUCCESS:CR_NOT_READY));
  }
  CHECK(Out.BootComplete && !Out.Outstanding && Out.BootIndex==4 && M.Kicks==4);
  UINT32 Calls=M.Calls;CHECK(CrAdrenoHfiBootPoll(&C,8,8,&Out)==CR_SUCCESS);CHECK(M.Calls==Calls);
}
static void RetainedFaults(void){
  ++Groups;Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);unsigned Calls=M.Calls;
  for(unsigned Mode=0;Mode<3;++Mode)for(unsigned At=1;At<=Calls;++At){
    Reset(TRUE);if(Mode==0)M.Fail=At;else if(Mode==1)M.Warn=At;else M.Seal=At;
    CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)!=CR_SUCCESS);QueryFault();
    if(At==Calls && Mode<2)CHECK(Out.Effects&CR_ADRENO_HFI_TX_PUBLISHED);
  }
  for(unsigned Mode=0;Mode<3;++Mode){
    Reset(FALSE);if(Mode==0)M.Fail=1;else if(Mode==1)M.Fail=2;else M.Fail=3;
    CHECK(CrAdrenoHfiPrepareQueues(&C)!=CR_SUCCESS);QueryFault();
  }
  for(unsigned Mode=0;Mode<3;++Mode){Reset(TRUE);if(Mode==0)M.Skip=1;else if(Mode==1)M.Double=1;else M.Denied=TRUE;
    CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)!=CR_SUCCESS);QueryFault();}
  Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);FirmwareConsume();Ack(C.Outcome.Header,0);
  M.Calls=0;CHECK(CrAdrenoHfiPoll(&C,1,8,&Out)==CR_SUCCESS);Calls=M.Calls;
  for(unsigned Mode=0;Mode<2;++Mode)for(unsigned At=1;At<=Calls;++At){
    Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);FirmwareConsume();Ack(C.Outcome.Header,0);
    M.Calls=0;if(Mode==0)M.Fail=At;else M.Warn=At;
    CHECK(CrAdrenoHfiPoll(&C,1,8,&Out)!=CR_SUCCESS);QueryFault();
    if(At==Calls)CHECK(Out.Acknowledged && (Out.Effects&CR_ADRENO_HFI_RX_PUBLISHED));
  }
}
static void RingWrapAndFull(void){
  ++Groups;Reset(TRUE);CHECK(CrAdrenoHfiBuildBootCommand(&Plan,1,&Cmd)==CR_SUCCESS);
  for(unsigned I=0;I<20;++I){UINT32 Old=C.TxIndex;
    CHECK(CrAdrenoHfiSubmit(&C,&Cmd,I*2,100,&Out)==CR_NOT_READY);
    CHECK(C.TxIndex==((Old+160)&1023));FirmwareConsume();Ack(C.Outcome.Header,0);
    CHECK(CrAdrenoHfiPoll(&C,I*2+1,8,&Out)==CR_SUCCESS);
  }
  /* Real committed messages with responses but delayed command read index fill
   * the queue; capacity must include padding, and waiting consumes no head. */
  Reset(TRUE);CHECK(CrAdrenoHfiBuildBootCommand(&Plan,0,&Cmd)==CR_SUCCESS);
  for(unsigned I=0;I<17;++I){CHECK(CrAdrenoHfiSubmit(&C,&Cmd,I*2,100,&Out)==CR_NOT_READY);
    Ack(C.Outcome.Header,0);CHECK(CrAdrenoHfiPoll(&C,I*2+1,8,&Out)==CR_SUCCESS);}
  CHECK(C.TxIndex==1020);unsigned K=M.Kicks;UINT32 Head=Memory[17];
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,3,&Cmd)==CR_SUCCESS);
  CHECK(CrAdrenoHfiSubmit(&C,&Cmd,40,100,&Out)==CR_NOT_READY);CHECK(M.Kicks==K && Memory[17]==Head);
  CHECK(!(Out.Effects&CR_ADRENO_HFI_TX_TOUCHED));FirmwareConsume();
  CHECK(CrAdrenoHfiPoll(&C,41,8,&Out)==CR_NOT_READY);CHECK(Memory[17]==0 && M.Kicks==K+1);
  CHECK(Memory[2044]==Out.Header && Memory[2045]==0xfafafafa);FirmwareConsume();Ack(Out.Header,0);
  CHECK(CrAdrenoHfiPoll(&C,42,8,&Out)==CR_SUCCESS);
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,0,&Cmd)==CR_SUCCESS);
}
static void Responses(void){
  ++Groups;Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);
  Ack(C.Outcome.Header^1,0);Ack(C.Outcome.Header^(1U<<20),0);
  for(unsigned I=0;I<5;++I)Push(2,101|(4<<8),I,0);
  CHECK(CrAdrenoHfiPoll(&C,1,4,&Out)==CR_NOT_READY);CHECK(Out.StaleMessages==2 && Out.DebugMessages==2);
  Ack(C.Outcome.Header,0);CHECK(CrAdrenoHfiPoll(&C,2,2,&Out)==CR_SUCCESS);CHECK(Out.Acknowledged);
  for(unsigned Mode=0;Mode<9;++Mode){Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);
    if(Mode==0)Ack(C.Outcome.Header,17);
    if(Mode==1)Push(1,100|(4<<8),31,0);
    if(Mode==2){Memory[2048]=0;Memory[29]=4;}
    if(Mode==3){Memory[2048]=126|(20<<8)|(1<<16);Memory[29]=4;}
    if(Mode==4){Memory[2048]=126|(2<<8)|(1<<16);Memory[29]=4;}
    if(Mode==5)Memory[29]=1024;
    if(Mode==6)Memory[29]=1;
    if(Mode==7)Memory[17]=4;
    if(Mode==8)Memory[16]=1000; /* claims unproduced command data */
    CHECK(CrAdrenoHfiPoll(&C,1,8,&Out)!=CR_SUCCESS);QueryFault();
  }
}
static void DeadlineAndSequence(void){
  ++Groups;Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,50,5,&Out)==CR_NOT_READY);
  CHECK(CrAdrenoHfiPoll(&C,54,1,&Out)==CR_NOT_READY);UINT32 Calls=M.Calls;
  CHECK(CrAdrenoHfiPoll(&C,55,1,&Out)==CR_TIMEOUT);CHECK(M.Calls==Calls && Out.Outstanding);
  Ack(C.Outcome.Header,0);QueryFault();
  Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,50,5,&Out)==CR_NOT_READY);
  CHECK(CrAdrenoHfiPoll(&C,49,1,&Out)==CR_INVALID_PARAMETER);QueryFault();
  Reset(TRUE);CHECK(CrAdrenoHfiBuildBootCommand(&Plan,3,&Cmd)==CR_SUCCESS);
  for(unsigned N=1;N<=4094;++N){CHECK(CrAdrenoHfiSubmit(&C,&Cmd,N*2,10,&Out)==CR_NOT_READY);
    CHECK((Out.Header>>20)==N);FirmwareConsume();Ack(Out.Header,0);
    CHECK(CrAdrenoHfiPoll(&C,N*2+1,1,&Out)==CR_SUCCESS);}
  Calls=M.Calls;CHECK(CrAdrenoHfiSubmit(&C,&Cmd,10000,10,&Out)==CR_OUT_OF_RESOURCES);CHECK(M.Calls==Calls);
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,0,&Cmd)==CR_SUCCESS);
}
static void BootFaultProgress(void){
  ++Groups;
  /* Every boot ACK is a distinct progression obligation. Firmware failure or
   * provider failure after consumption must never submit the following stage. */
  for(unsigned Mode=0;Mode<3;++Mode)for(unsigned Stage=0;Stage<4;++Stage){
    Reset(TRUE);CHECK(CrAdrenoHfiBootBegin(&C,&Plan,0,100,&Out)==CR_NOT_READY);
    for(unsigned N=0;N<Stage;++N){FirmwareConsume();Ack(C.Outcome.Header,0);
      CHECK(CrAdrenoHfiBootPoll(&C,N+1,8,&Out)==CR_NOT_READY);}
    unsigned K=M.Kicks;FirmwareConsume();Ack(C.Outcome.Header,Mode==0?23:0);M.Calls=0;
    if(Mode==1)M.Fail=4; /* Execute after acquire/order/response publication. */
    CR_STATUS S=CrAdrenoHfiBootPoll(&C,Mode==2?200:Stage+1,8,&Out);
    CHECK(S!=CR_SUCCESS && S!=CR_NOT_READY);CHECK(!Out.BootComplete && Out.BootIndex==Stage && M.Kicks==K);
    QueryFault();
  }
  /* Queue-space timeout is bounded by the original submission, never renewed
   * by polls. Fill with acknowledged commands whose hardware read lags. */
  Reset(TRUE);for(unsigned N=0;N<17;++N){CHECK(CrAdrenoHfiSubmit(&C,&Cmd,N,100,&Out)==CR_NOT_READY);
    Ack(C.Outcome.Header,0);CHECK(CrAdrenoHfiPoll(&C,N,1,&Out)==CR_SUCCESS);}
  CHECK(CrAdrenoHfiSubmit(&C,&Cmd,20,5,&Out)==CR_NOT_READY);CHECK(!(Out.Effects&CR_ADRENO_HFI_TX_TOUCHED));
  CHECK(CrAdrenoHfiPoll(&C,24,1,&Out)==CR_NOT_READY);unsigned K=M.Kicks;
  CHECK(CrAdrenoHfiPoll(&C,25,1,&Out)==CR_TIMEOUT);CHECK(M.Kicks==K);QueryFault();
}
static void EmptyRequest(void){
  ++Groups;CHECK(OracleEmptyRequest()==1);Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);
  Memory[26]=0;CHECK(CrAdrenoHfiPoll(&C,1,1,&Out)==CR_NOT_READY);CHECK(Memory[26]==1);
  Reset(TRUE);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,100,&Out)==CR_NOT_READY);
  Memory[26]=0;M.Race=1;CHECK(CrAdrenoHfiPoll(&C,1,1,&Out)==CR_SUCCESS);
  CHECK(!M.Race && Out.Acknowledged && !Out.Outstanding);
}
static void Hostile(void){
  ++Groups;Reset(TRUE);CR_ADRENO_HFI_OUTCOME Sentinel;memset(&Sentinel,0x72,sizeof(Sentinel));Out=Sentinel;
  CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,0,&Out)==CR_INVALID_PARAMETER);CHECK(!memcmp(&Out,&Sentinel,sizeof(Out)));
  CHECK(CrAdrenoHfiSubmit(&C,&Cmd,~(UINT64)0,1,&Out)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoHfiPoll(&C,0,33,&Out)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoHfiQuery(&C,(void *)&C)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoHfiQuery(&C,(void *)Memory)==CR_INVALID_PARAMETER);
  CR_ADRENO_HFI_COMMAND Bad=Cmd;Bad.Count=161;CHECK(CrAdrenoHfiSubmit(&C,&Bad,0,10,&Out)==CR_INVALID_PARAMETER);
  Bad=Cmd;Bad.Words[159]=1;CHECK(CrAdrenoHfiSubmit(&C,&Bad,0,10,&Out)==CR_INVALID_PARAMETER);
  Bad=Cmd;Bad.Words[1]=17;CHECK(CrAdrenoHfiSubmit(&C,&Bad,0,10,&Out)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,1,&Bad)==CR_SUCCESS);Bad.Words[25]=Bad.Words[24];
  CHECK(CrAdrenoHfiSubmit(&C,&Bad,0,10,&Out)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoHfiBuildBootCommand(&Plan,1,&Bad)==CR_SUCCESS);Bad.Words[159]=1;
  CHECK(CrAdrenoHfiSubmit(&C,&Bad,0,10,&Out)==CR_INVALID_PARAMETER);
  CR_ADRENO_HFI_CONTEXT Copy=C;CHECK(CrAdrenoHfiPoll(&Copy,0,8,&Out)==CR_INVALID_PARAMETER);
  Backing.GmuIova+=4096;CHECK(CrAdrenoHfiPoll(&C,0,8,&Out)==CR_INVALID_PARAMETER);Backing.GmuIova-=4096;
  for(unsigned Mode=0;Mode<7;++Mode){CR_ADRENO_HFI_BOOT_PLAN B=Plan;
    if(Mode==0)B.AcdEnabled=1;if(Mode==1)B.IfpcEnabled=1;if(Mode==2)B.BcmAddress[1]=B.BcmAddress[0];
    if(Mode==3)B.BcmAddress[1]=0;if(Mode==4)B.BcmAddress[1]=0x01000000;
    if(Mode==5)B.Power.Gx[15].Vote=1;if(Mode==6)B.Power.GxCount=17;
    CHECK(CrAdrenoHfiBootBegin(&C,&B,0,10,&Out)!=CR_SUCCESS);CHECK(M.Calls==0);
  }
  CHECK(CrAdrenoHfiDrain(&C)==CR_NOT_READY);CHECK(CrAdrenoHfiSeal(&C)==CR_SUCCESS);
  CHECK(CrAdrenoHfiDrain(&C)==CR_SUCCESS);CHECK(CrAdrenoHfiSubmit(&C,&Cmd,0,10,&Out)==CR_ABORT);
}
static void *Sender(void *Unused){(void)Unused;ThreadStatus=CrAdrenoHfiSubmit(&C,&Cmd,0,10,&Out);return NULL;}
static void ThreadedSeal(void){
  ++Groups;Reset(TRUE);M.Pause=4;Paused=Resume=0;pthread_t Thread;
  CHECK(!pthread_create(&Thread,NULL,Sender,NULL));pthread_mutex_lock(&Lock);
  while(!Paused)pthread_cond_wait(&Cond,&Lock);
  CHECK(CrAdrenoHfiSeal(&C)==CR_BUSY);CHECK(CrAdrenoHfiDrain(&C)==CR_BUSY);
  CR_ADRENO_HFI_OUTCOME Other;CHECK(CrAdrenoHfiQuery(&C,&Other)==CR_BUSY);
  Resume=1;pthread_cond_broadcast(&Cond);pthread_mutex_unlock(&Lock);CHECK(!pthread_join(Thread,NULL));
  CHECK(ThreadStatus!=CR_SUCCESS);CHECK(CrAdrenoHfiDrain(&C)==CR_SUCCESS);
  CHECK(CrAdrenoHfiQuery(&C,&Other)==CR_SUCCESS);CHECK(Other.Faulted && Other.Outstanding);
}
int main(void){Oracle();Boot();RetainedFaults();RingWrapAndFull();Responses();DeadlineAndSequence();BootFaultProgress();EmptyRequest();Hostile();ThreadedSeal();
  printf("AdrenoHfi PASS: %u groups, %u checks; actual core + source wire oracle\n",Groups,Checks);return 0;}
