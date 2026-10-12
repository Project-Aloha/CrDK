/* Retained bounded HFI transport. SPDX-License-Identifier: GPL-2.0-only
 * Wire layout: Linux a6xx_hfi.c/h a53261b6, Copyright2017-2018 Linux Foundation.
 * New admitted publication, response validation and failure/lifetime state. */
#include "Internal.h"
#include <oskal/cr_atomic.h>
#define RING_WORDS 1024U
#define HEADER_WORDS 12U
#define TABLE_WORDS 6U
#define INDEX_READ 10U
#define INDEX_WRITE 11U
STATIC BOOLEAN Span(CONST VOID *P,UINTN N,UINTN A) {
  return P && N && (UINTN)P%A==0 && (UINTN)P<=~(UINTN)0-N;
}
STATIC BOOLEAN Overlap(CONST VOID *A,UINTN An,CONST VOID *B,UINTN Bn) {
  return (UINTN)A<(UINTN)B+Bn && (UINTN)B<(UINTN)A+An;
}
STATIC UINT32 Compare(volatile UINT32 *P,UINT32 Old,UINT32 New) {
#ifdef _KERNEL_MODE
  return (UINT32)InterlockedCompareExchange((volatile LONG *)P,(LONG)New,(LONG)Old);
#else
  return InterlockedCompareExchange32(P,Old,New);
#endif
}
STATIC BOOLEAN Identity(CR_ADRENO_HFI_CONTEXT *C) {
  if(!Span(C,sizeof(*C),8)||C->Self!=C || !Span(C->OriginalIo,sizeof(*C->OriginalIo),8) ||
      !Span(C->OriginalBacking,sizeof(*C->OriginalBacking),8))return FALSE;
  CONST CR_ADRENO_HFI_IO *I=C->OriginalIo;CONST CR_ADRENO_HFI_BACKING *B=C->OriginalBacking;
  return I->Context==C->Io.Context && I->ContextBytes==C->Io.ContextBytes &&
    I->StartIncarnation==C->Io.StartIncarnation && I->Execute==C->Io.Execute &&
    I->Barrier==C->Io.Barrier && I->Kick==C->Io.Kick &&
    B->Cpu==C->Backing.Cpu && B->Bytes==C->Backing.Bytes && B->GmuIova==C->Backing.GmuIova;
}
STATIC BOOLEAN Separate(CR_ADRENO_HFI_CONTEXT *C,CONST VOID *P,UINTN N,UINTN A) {
  return Span(P,N,A) && !Overlap(P,N,C,sizeof(*C)) &&
    !Overlap(P,N,C->OriginalIo,sizeof(*C->OriginalIo)) &&
    !Overlap(P,N,C->OriginalBacking,sizeof(*C->OriginalBacking)) &&
    !Overlap(P,N,C->Io.Context,C->Io.ContextBytes) &&
    !Overlap(P,N,(CONST VOID *)C->Backing.Cpu,C->Backing.Bytes);
}
STATIC CR_STATUS Live(CR_ADRENO_HFI_CONTEXT *C) {
  if(!Identity(C))return CR_INVALID_PARAMETER;
  return (CrAtomicLoad32(&C->Gate)&CR_ADRENO_HFI_CLOSED)?CR_ABORT:
    C->Faulted?C->Outcome.FirstFailure:CR_SUCCESS;
}
STATIC CR_STATUS Enter(CR_ADRENO_HFI_CONTEXT *C,BOOLEAN Query) {
  if(!Identity(C))return CR_INVALID_PARAMETER;
  UINT32 Old=CrAtomicLoad32(&C->Gate);
  if(Old&CR_ADRENO_HFI_ACTIVE)return CR_BUSY;
  if(!Query){CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;}
  return Compare(&C->Gate,Old,Old|CR_ADRENO_HFI_ACTIVE)==Old?CR_SUCCESS:CR_BUSY;
}
STATIC CR_STATUS Fault(CR_ADRENO_HFI_CONTEXT *C,CR_STATUS S) {
  if(S==CR_SUCCESS || !CR_ERROR(S))S=CR_DEVICE_ERROR;
  C->Faulted=1;C->Outcome.Faulted=1;C->Outcome.LastStatus=S;
  if(C->Outcome.FirstFailure==CR_SUCCESS)C->Outcome.FirstFailure=S;
  return S;
}
STATIC CR_STATUS Finish(CR_ADRENO_HFI_CONTEXT *C,CR_STATUS S,CR_ADRENO_HFI_OUTCOME *O) {
  if(!C->Faulted){CR_STATUS L=Live(C);if(L!=CR_SUCCESS)S=Fault(C,L);}
  C->Outcome.LastStatus=S;if(O)*O=C->Outcome;
  CrAtomicAnd32(&C->Gate,~CR_ADRENO_HFI_ACTIVE);return S;
}
STATIC CR_STATUS Barrier(CR_ADRENO_HFI_CONTEXT *C,CR_ADRENO_HFI_ORDER O) {
  CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;
  S=C->Io.Barrier(C->Io.Context,C->Io.StartIncarnation,O);
  return S==CR_SUCCESS?Live(C):Fault(C,S);
}
STATIC volatile UINT32 *Header(CR_ADRENO_HFI_CONTEXT *C,UINT32 Q) {
  return C->Backing.Cpu+TABLE_WORDS+Q*HEADER_WORDS;
}
STATIC volatile UINT32 *Ring(CR_ADRENO_HFI_CONTEXT *C,UINT32 Q) {
  return C->Backing.Cpu+(Q+1)*RING_WORDS;
}
STATIC CR_STATUS Shape(CR_ADRENO_HFI_CONTEXT *C) {
  volatile UINT32 *T=C->Backing.Cpu;
  if(T[0]!=0||T[1]!=168||T[2]!=6||T[3]!=12||T[4]!=3||T[5]!=3)return CR_COMPROMISED_DATA;
  for(UINT32 Q=0;Q<3;++Q){
    volatile UINT32 *H=Header(C,Q);
    if(H[0]!=1 || H[1]!=C->Backing.GmuIova+(Q+1)*4096 || H[2]!=(0xa00U|Q) ||
       H[3]!=1024 || H[4]!=0 || H[6]!=1 || H[7]!=1)return CR_COMPROMISED_DATA;
    UINT32 R=H[INDEX_READ],W=H[INDEX_WRITE];
    if(R>=1024 || W>=1024 || (R&3) || (W&3))return CR_COMPROMISED_DATA;
    if((Q==0 && W!=C->TxIndex) || (Q && R!=C->RxIndex[Q-1]))return CR_COMPROMISED_DATA;
    if(Q==0){
      if(((R-C->TxReadObserved)&1023)>((C->TxIndex-C->TxReadObserved)&1023))return CR_COMPROMISED_DATA;
      C->TxReadObserved=R;
    }
  }
  return CR_SUCCESS;
}
typedef struct {CR_ADRENO_HFI_EVENT *Events;UINT32 Capacity,Count,Probes,More;} EVENTS;
typedef struct {CR_ADRENO_HFI_CONTEXT *C;UINT32 Calls,Budget;CR_ADRENO_HFI_ACCESS Access;
  CR_STATUS Result;EVENTS *Events;} CALL;
STATIC CR_STATUS Prepare(CR_ADRENO_HFI_CONTEXT *C) {
  C->Outcome.Effects|=CR_ADRENO_HFI_PREPARE_TOUCHED;
  for(UINT32 N=0;N<CR_ADRENO_HFI_BYTES/4;++N)C->Backing.Cpu[N]=0;
  volatile UINT32 *T=C->Backing.Cpu;T[1]=168;T[2]=6;T[3]=12;T[4]=3;T[5]=3;
  for(UINT32 Q=0;Q<3;++Q){volatile UINT32 *H=Header(C,Q);
    H[0]=1;H[1]=C->Backing.GmuIova+(Q+1)*4096;H[2]=0xa00U|Q;H[3]=1024;
    H[6]=1;H[7]=1;H[8]=1;
  }
  CR_STATUS S=Barrier(C,CrAdrenoHfiRelease);if(S!=CR_SUCCESS)return S;
  C->Prepared=1;C->Outcome.Effects|=CR_ADRENO_HFI_PREPARED;return CR_SUCCESS;
}
STATIC CR_STATUS Transmit(CR_ADRENO_HFI_CONTEXT *C) {
  CR_STATUS S=Barrier(C,CrAdrenoHfiAcquire);if(S!=CR_SUCCESS)return S;
  S=Shape(C);if(S!=CR_SUCCESS)return S;
  volatile UINT32 *H=Header(C,0),*R=Ring(C,0);
  UINT32 Read=H[INDEX_READ],At=C->TxIndex,Count=(C->Command.Count+3)&~3U;
  if(((Read-At-1)&1023)<Count)return CR_NOT_READY;
  C->Outcome.Effects|=CR_ADRENO_HFI_TX_TOUCHED;
  for(UINT32 N=0;N<Count;++N){R[At]=N==0?C->Outcome.Header:
      N<C->Command.Count?C->Command.Words[N]:0xfafafafaU;At=(At+1)&1023;}
  S=Barrier(C,CrAdrenoHfiRelease);if(S!=CR_SUCCESS)return S;
  H[INDEX_WRITE]=At;C->TxIndex=At;C->Outcome.Effects|=CR_ADRENO_HFI_TX_PUBLISHED;
  /* Head must become visible before the device notification. */
  S=Barrier(C,CrAdrenoHfiRelease);if(S!=CR_SUCCESS)return S;
  C->Outcome.Effects|=CR_ADRENO_HFI_KICK_ATTEMPTED;
  S=C->Io.Kick(C->Io.Context,C->Io.StartIncarnation);
  if(S!=CR_SUCCESS)return S;
  C->Outcome.Effects|=CR_ADRENO_HFI_KICK_ACKNOWLEDGED;return CR_NOT_READY;
}
STATIC VOID Increment(UINT32 *V){if(*V!=0xffffffffU)++*V;}
STATIC CR_STATUS ReceiveOne(CR_ADRENO_HFI_CONTEXT *C,UINT32 Q,BOOLEAN *Consumed,EVENTS *Events) {
  *Consumed=FALSE;CR_STATUS S=Barrier(C,CrAdrenoHfiAcquire);if(S!=CR_SUCCESS)return S;
  S=Shape(C);if(S!=CR_SUCCESS)return S;
  volatile UINT32 *H=Header(C,Q),*R=Ring(C,Q);
  UINT32 At=C->RxIndex[Q-1],Write=H[INDEX_WRITE],Available=(Write-At)&1023;
  if(!Available){
    /* Source queue_read rearms an empty queue's receive notification. Publish
     * that request before rechecking head, so a racing producer cannot leave
     * this call asleep with an already committed response. */
    S=Barrier(C,CrAdrenoHfiRelease);if(S!=CR_SUCCESS)return S;
    H[8]=1;C->Outcome.Effects|=CR_ADRENO_HFI_RX_REQUESTED;
    S=Barrier(C,CrAdrenoHfiRelease);if(S!=CR_SUCCESS)return S;
    S=Barrier(C,CrAdrenoHfiAcquire);if(S!=CR_SUCCESS)return S;
    S=Shape(C);if(S!=CR_SUCCESS)return S;
    Write=H[INDEX_WRITE];Available=(Write-At)&1023;
    if(!Available)return CR_NOT_READY;
  }
  UINT32 First=R[At],Count=(First>>8)&255U,Padded=(Count+3)&~3U;
  if(!Count || Padded>Available)return CR_COMPROMISED_DATA;
  UINT32 Id=First&255,Type=(First>>16)&15,Ret=0,Error=0;BOOLEAN Ack=FALSE;
  if(Q==1){
    if(Id==126){
      if(Type!=1 || Count<3 || Count>19)return CR_COMPROMISED_DATA;
      Ret=R[(At+1)&1023];Error=R[(At+2)&1023];
      Ack=TO_BOOL(C->Outcome.Outstanding && (C->Outcome.Effects&CR_ADRENO_HFI_TX_PUBLISHED) &&
        Ret==C->Outcome.Header);
    }else if(Id==100){if(Count!=4)return CR_COMPROMISED_DATA;Error=R[(At+1)&1023];}
    else return CR_COMPROMISED_DATA;
  }
  /* Snapshot before tail publication permits firmware to reuse its ring.
   * Capacity is checked by the bounded caller, never drop an unreported event. */
  if(Events){
    CR_ADRENO_HFI_EVENT *E=&Events->Events[Events->Count];
    E->Kind=Q==2?CrAdrenoHfiEventDebug:Id==100?CrAdrenoHfiEventFirmwareError:
      Ack?CrAdrenoHfiEventAck:CrAdrenoHfiEventStaleAck;
    E->Queue=Q;E->Count=Count;
    for(UINT32 N=0;N<CR_ADRENO_HFI_EVENT_WORDS;++N)E->Words[N]=N<Count?R[(At+N)&1023]:0;
  }
  S=Barrier(C,CrAdrenoHfiRelease);if(S!=CR_SUCCESS)return S;
  At=(At+Padded)&1023;H[INDEX_READ]=At;C->RxIndex[Q-1]=At;
  C->Outcome.Effects|=CR_ADRENO_HFI_RX_PUBLISHED;*Consumed=TRUE;
  if(Events)++Events->Count;
  if(Q==2){Increment(&C->Outcome.DebugMessages);C->Outcome.LastDebugHeader=First;return CR_SUCCESS;}
  if(Id==100){C->Outcome.FirmwareError=Error;return CR_DEVICE_ERROR;}
  if(!Ack){Increment(&C->Outcome.StaleMessages);return CR_SUCCESS;}
  C->Outcome.Acknowledged=1;C->Outcome.FirmwareError=Error;
  C->Outcome.Effects|=CR_ADRENO_HFI_ACK_RECEIVED;
  if(Error)return CR_DEVICE_ERROR;
  C->Outcome.Outstanding=0;return CR_SUCCESS;
}
STATIC CR_STATUS Receive(CR_ADRENO_HFI_CONTEXT *C,UINT32 Budget) {
  /* One response then one debug message per cycle; empty queues consume a
   * bounded probe too, preventing an active debug producer from starving ACK. */
  for(UINT32 N=0;N<Budget;++N){
    BOOLEAN Used;CR_STATUS S=ReceiveOne(C,(N&1)?2:1,&Used,NULL);
    if(S!=CR_SUCCESS && S!=CR_NOT_READY)return S;
    if(C->Outcome.Acknowledged)return CR_SUCCESS;
  }
  return CR_NOT_READY;
}
STATIC CR_STATUS ReceiveRuntime(CR_ADRENO_HFI_CONTEXT *C,UINT32 Budget,EVENTS *E) {
  UINT32 Empty=0;
  for(UINT32 N=0;N<Budget;++N){
    if(E->Count==E->Capacity){E->More=1;break;}
    BOOLEAN Used;UINT32 Q=1+(C->RuntimeNextQueue&1U);
    CR_STATUS S=ReceiveOne(C,Q,&Used,E);C->RuntimeNextQueue^=1U;++E->Probes;
    if(S!=CR_SUCCESS && S!=CR_NOT_READY)return S;
    Empty=Used?0:Empty+1;
    if(Empty==2)return CR_SUCCESS;
  }
  if(E->Probes==Budget)E->More=1;
  return CR_SUCCESS;
}
STATIC CR_STATUS Operation(VOID *Argument) {
  CALL *A=Argument;if(++A->Calls!=1){A->Result=CR_DEVICE_ERROR;return A->Result;}
  CR_STATUS S=Live(A->C);if(S==CR_SUCCESS){
    S=A->Access==CrAdrenoHfiPrepare?Prepare(A->C):
      A->Access==CrAdrenoHfiTransmit?Transmit(A->C):A->Events?ReceiveRuntime(A->C,A->Budget,A->Events):Receive(A->C,A->Budget);
  }
  A->Result=S;return S;
}
STATIC CR_STATUS Execute(CR_ADRENO_HFI_CONTEXT *C,CR_ADRENO_HFI_ACCESS Access,UINT32 Budget,EVENTS *Events) {
  CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return Fault(C,S);
  CALL A={C,0,Budget,Access,CR_DEVICE_ERROR,Events};
  S=C->Io.Execute(C->Io.Context,C->Io.StartIncarnation,Access,Operation,&A);
  /* NOT_READY is a successful admitted pending operation, not a provider's
   * positive warning. Require exact callback count and exact result propagation. */
  if(A.Calls!=1 || S!=A.Result || (S!=CR_SUCCESS && S!=CR_NOT_READY))return Fault(C,S);
  CR_STATUS L=Live(C);if(L!=CR_SUCCESS)return Fault(C,L);
  return S;
}
STATIC CR_STATUS Start(CR_ADRENO_HFI_CONTEXT *C,CONST CR_ADRENO_HFI_COMMAND *M,
    UINT64 Now,UINT32 Timeout) {
  if(!C->Prepared)return CR_NOT_READY;
  if(C->Outcome.Outstanding)return CR_BUSY;
  if(C->NextSequence>=4094)return CR_OUT_OF_RESOURCES;
  if(Now<C->LastNowUs)return Fault(C,CR_INVALID_PARAMETER);
  C->Command=*M;C->LastNowUs=Now;C->Outcome.DeadlineUs=Now+Timeout;
  ++C->NextSequence;++C->Outcome.Transaction;
  C->Outcome.Header=(C->NextSequence<<20)|(M->Count<<8)|M->Id;
  C->Outcome.Outstanding=1;C->Outcome.Acknowledged=0;C->Outcome.FirmwareError=0;
  C->Outcome.Effects&=CR_ADRENO_HFI_PREPARE_TOUCHED|CR_ADRENO_HFI_PREPARED;
  return Execute(C,CrAdrenoHfiTransmit,0,NULL);
}
STATIC CR_STATUS Progress(CR_ADRENO_HFI_CONTEXT *C,UINT64 Now,UINT32 Budget) {
  if(Now<C->LastNowUs)return Fault(C,CR_INVALID_PARAMETER);
  C->LastNowUs=Now;
  if(!C->Outcome.Outstanding)return C->Outcome.Acknowledged?CR_SUCCESS:CR_NOT_READY;
  if(Now>=C->Outcome.DeadlineUs)return Fault(C,CR_TIMEOUT);
  if(!(C->Outcome.Effects&CR_ADRENO_HFI_TX_PUBLISHED))return Execute(C,CrAdrenoHfiTransmit,0,NULL);
  return Execute(C,CrAdrenoHfiReceive,Budget,NULL);
}
CR_STATUS CrAdrenoHfiInit(CR_ADRENO_HFI_CONTEXT *C,CONST CR_ADRENO_HFI_IO *I,
    CONST CR_ADRENO_HFI_BACKING *B,UINT64 Epoch) {
  if(!Span(C,sizeof(*C),8)||!Span(I,sizeof(*I),8)||!Span(B,sizeof(*B),8))return CR_INVALID_PARAMETER;
  CONST VOID *P[5]={C,I,B,I->Context,(CONST VOID *)B->Cpu};
  UINTN N[5]={sizeof(*C),sizeof(*I),sizeof(*B),I->ContextBytes,B->Bytes};
  if(!Epoch || I->StartIncarnation!=Epoch || !I->Execute || !I->Barrier || !I->Kick ||
      !Span(I->Context,I->ContextBytes,1) || !Span((CONST VOID *)B->Cpu,B->Bytes,4096) ||
      B->Bytes!=16384 || !B->GmuIova || (B->GmuIova&4095) || B->GmuIova>0xffffc000U)
    return CR_INVALID_PARAMETER;
  for(UINT32 A=0;A<5;++A)for(UINT32 D=0;D<A;++D)if(Overlap(P[A],N[A],P[D],N[D]))return CR_INVALID_PARAMETER;
  if(C->Self || CrAtomicLoad32(&C->Gate))return CR_ALREADY_STARTED;
  /* Caller promises initially zeroed permanent storage, not a reinit mechanism. */
  C->Self=C;C->OriginalIo=I;C->OriginalBacking=B;C->Io=*I;C->Backing=*B;
  C->Outcome.Version=1;C->Outcome.Size=sizeof(C->Outcome);C->Outcome.StartIncarnation=Epoch;
  return CR_SUCCESS;
}
CR_STATUS CrAdrenoHfiPrepareQueues(CR_ADRENO_HFI_CONTEXT *C) {
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  if(C->PrepareAttempted)return Finish(C,CR_ALREADY_STARTED,NULL);
  C->PrepareAttempted=1;return Finish(C,Execute(C,CrAdrenoHfiPrepare,0,NULL),NULL);
}
STATIC BOOLEAN Arguments(CR_ADRENO_HFI_CONTEXT *C,CR_ADRENO_HFI_OUTCOME *O) {
  return Identity(C) && Separate(C,O,sizeof(*O),8);
}
CR_STATUS CrAdrenoHfiSubmit(CR_ADRENO_HFI_CONTEXT *C,CONST CR_ADRENO_HFI_COMMAND *M,
    UINT64 Now,UINT32 Timeout,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O) || !Separate(C,M,sizeof(*M),4) || Overlap(M,sizeof(*M),O,sizeof(*O)) ||
      !Timeout || Timeout>1000000 || Now>~(UINT64)0-Timeout || !AdrenoHfiCommandValid(M))return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  S=(C->Outcome.BootActive||C->Outcome.RuntimeActive)?CR_BUSY:Start(C,M,Now,Timeout);return Finish(C,S,O);
}
CR_STATUS CrAdrenoHfiPoll(CR_ADRENO_HFI_CONTEXT *C,UINT64 Now,UINT32 Budget,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O)||!Budget||Budget>32)return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  S=(C->Outcome.BootActive||C->Outcome.RuntimeActive)?CR_BUSY:Progress(C,Now,Budget);return Finish(C,S,O);
}
CR_STATUS CrAdrenoHfiBootBegin(CR_ADRENO_HFI_CONTEXT *C,CONST CR_ADRENO_HFI_BOOT_PLAN *B,
    UINT64 Now,UINT32 Timeout,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O)||!Separate(C,B,sizeof(*B),4)||Overlap(B,sizeof(*B),O,sizeof(*O)) ||
      !Timeout || Timeout>1000000 || Now>~(UINT64)0-Timeout)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrAdrenoHfiValidateBoot(B);if(S!=CR_SUCCESS)return S;
  S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  if(C->Outcome.BootActive || C->Outcome.Transaction || !C->Prepared)return Finish(C,CR_BUSY,O);
  C->Boot=*B;C->BootTimeoutUs=Timeout;C->Outcome.BootActive=1;C->Outcome.BootIndex=0;
  CR_ADRENO_HFI_COMMAND M;S=CrAdrenoHfiBuildBootCommand(&C->Boot,0,&M);
  if(S==CR_SUCCESS)S=Start(C,&M,Now,Timeout);return Finish(C,S,O);
}
CR_STATUS CrAdrenoHfiBootPoll(CR_ADRENO_HFI_CONTEXT *C,UINT64 Now,UINT32 Budget,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O)||!Budget||Budget>32)return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  if(!C->Outcome.BootActive)return Finish(C,CR_NOT_READY,O);
  if(C->Outcome.BootComplete)return Finish(C,CR_SUCCESS,O);
  S=Progress(C,Now,Budget);
  if(S==CR_SUCCESS){
    if(++C->Outcome.BootIndex==4)C->Outcome.BootComplete=1;
    else if(Now>~(UINT64)0-C->BootTimeoutUs)S=Fault(C,CR_INVALID_PARAMETER);
    else{CR_ADRENO_HFI_COMMAND M;S=CrAdrenoHfiBuildBootCommand(&C->Boot,C->Outcome.BootIndex,&M);
      if(S==CR_SUCCESS)S=Start(C,&M,Now,C->BootTimeoutUs);else S=Fault(C,S);}
  }
  return Finish(C,S,O);
}
CR_STATUS CrAdrenoHfiQuery(CR_ADRENO_HFI_CONTEXT *C,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O))return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C,TRUE);if(S!=CR_SUCCESS)return S;
  *O=C->Outcome;CrAtomicAnd32(&C->Gate,~CR_ADRENO_HFI_ACTIVE);return CR_SUCCESS;
}
CR_STATUS CrAdrenoHfiSeal(CR_ADRENO_HFI_CONTEXT *C) {
  if(!Span(C,sizeof(*C),8)||C->Self!=C)return CR_INVALID_PARAMETER;
  return (CrAtomicOr32(&C->Gate,CR_ADRENO_HFI_CLOSED)&CR_ADRENO_HFI_ACTIVE)?CR_BUSY:CR_SUCCESS;
}
CR_STATUS CrAdrenoHfiDrain(CR_ADRENO_HFI_CONTEXT *C) {
  if(!Span(C,sizeof(*C),8)||C->Self!=C)return CR_INVALID_PARAMETER;
  UINT32 G=CrAtomicLoad32(&C->Gate);
  return !(G&CR_ADRENO_HFI_CLOSED)?CR_NOT_READY:(G&CR_ADRENO_HFI_ACTIVE)?CR_BUSY:CR_SUCCESS;
}

CR_STATUS CrAdrenoHfiEnterRuntime(CR_ADRENO_HFI_CONTEXT *C,UINT32 Version,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O))return CR_INVALID_PARAMETER;
  if(Version!=CR_ADRENO_HFI_A730_VERSION)return CR_UNSUPPORTED;
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  if(C->Outcome.RuntimeActive)return Finish(C,CR_ALREADY_STARTED,O);
  if(!C->Prepared||!C->Outcome.BootActive||!C->Outcome.BootComplete||
      C->Outcome.BootIndex!=4||C->Outcome.Outstanding||!C->Outcome.Acknowledged)
    return Finish(C,CR_NOT_READY,O);
  C->Outcome.RuntimeActive=1;C->Outcome.FirmwareHfiVersion=Version;
  C->Outcome.BootActive=0;return Finish(C,CR_SUCCESS,O);
}
CR_STATUS CrAdrenoHfiService(CR_ADRENO_HFI_CONTEXT *C,UINT64 Now,UINT32 Budget,
    CR_ADRENO_HFI_EVENT *Events,UINT32 Capacity,CR_ADRENO_HFI_SERVICE_RESULT *Result) {
  if(!Identity(C)||!Budget||Budget>32||!Capacity||Capacity>32||
      !Separate(C,Result,sizeof(*Result),8)||!Separate(C,Events,Capacity*sizeof(*Events),4)||
      Overlap(Result,sizeof(*Result),Events,Capacity*sizeof(*Events)))return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  EVENTS E={Events,Capacity,0,0,0};
  if(!C->Outcome.RuntimeActive)S=CR_NOT_READY;
  else if(Now<C->LastNowUs)S=Fault(C,CR_INVALID_PARAMETER);
  else {
    C->LastNowUs=Now;
    if(C->Outcome.Outstanding&&Now>=C->Outcome.DeadlineUs)S=Fault(C,CR_TIMEOUT);
    else {
      /* Drain even with a TX waiting for ring space, so asynchronous failure
       * cannot starve behind it. Do not replay an already published command. */
      S=Execute(C,CrAdrenoHfiReceive,Budget,&E);
      if(S==CR_SUCCESS&&C->Outcome.Outstanding&&!(C->Outcome.Effects&CR_ADRENO_HFI_TX_PUBLISHED))
        S=Execute(C,CrAdrenoHfiTransmit,0,NULL);
      if(S==CR_SUCCESS&&C->Outcome.Outstanding)S=CR_NOT_READY;
    }
  }
  Result->Count=E.Count;Result->Probes=E.Probes;Result->More=E.More;
  return Finish(C,S,&Result->Outcome);
}
STATIC CR_STATUS RuntimeCommand(CR_ADRENO_HFI_CONTEXT *C,UINT32 Id,UINT32 Gx,UINT32 Bw,
    UINT64 Now,UINT32 Timeout,CR_ADRENO_HFI_OUTCOME *O) {
  if(!Arguments(C,O)||!Timeout||Timeout>1000000||Now>~(UINT64)0-Timeout)return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C,FALSE);if(S!=CR_SUCCESS)return S;
  if(!C->Outcome.RuntimeActive)return Finish(C,CR_NOT_READY,O);
  if(Id==30&&(Gx==0||Gx>=C->Boot.Power.GxCount||Bw==0||Bw>=12))return Finish(C,CR_INVALID_PARAMETER,O);
  CR_ADRENO_HFI_COMMAND M={0};M.Id=Id;M.Count=Id==33?3:4;
  if(Id==30){M.Words[1]=1;M.Words[2]=Gx;M.Words[3]=Bw;}
  return Finish(C,Start(C,&M,Now,Timeout),O);
}
CR_STATUS CrAdrenoHfiRuntimeVote(CR_ADRENO_HFI_CONTEXT *C,UINT32 Gx,UINT32 Bw,
    UINT64 Now,UINT32 Timeout,CR_ADRENO_HFI_OUTCOME *O) {
  return RuntimeCommand(C,30,Gx,Bw,Now,Timeout,O);
}
CR_STATUS CrAdrenoHfiPrepareSlumber(CR_ADRENO_HFI_CONTEXT *C,UINT64 Now,UINT32 Timeout,
    CR_ADRENO_HFI_OUTCOME *O) {return RuntimeCommand(C,33,0,0,Now,Timeout,O);}
