/* Actual stop engine/target/resolver with a separate device model. GPL-2.0-only */
#include <Library/AdrenoStopCrLib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include "AdrenoStopReference.h"
static _Atomic unsigned Checks;
#define CHECK(X) do{++Checks;if(!(X)){fprintf(stderr,"%s:%d %s\n",__FILE__,__LINE__,#X);abort();}}while(0)
#ifndef _KERNEL_MODE
#include <Library/SynchronizationLib.h>
UINT32 EFIAPI InterlockedCompareExchange32(volatile UINT32 *P,UINT32 Old,UINT32 New){
  __atomic_compare_exchange_n(P,&Old,New,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);return Old;
}
#endif
#define BIT64(I) (((UINT64)1)<<(I))
typedef struct {
  CR_SM8450_GPU_LAYOUT Layout;
  UINT32 Reg[CrAdrenoStopRegisterCount];
  UINT32 Calls,Reads,Writes,Delays,Barriers,ClearCommands,ClientWrites,ArbWrites;
  UINT32 FailAt,AfterWriteFailAt,WarnAt,SealAt,BusyAt,MutateAt,Stuck,Drop;
  UINT32 GxDenied,SeenTcs,DelayUs,ResetDelayUs,PauseAt;
  UINT64 Written;
  CR_ADRENO_STOP_CONTEXT *Owner;
  CR_STATUS SealResult;
  BOOLEAN BadAck,EmptyRead;
} MODEL;
static MODEL M;
static CR_ADRENO_STOP_CONTEXT C;
static CR_ADRENO_STOP_IO Io;
static pthread_mutex_t PauseLock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t PauseCond=PTHREAD_COND_INITIALIZER;
static int Paused,Resume;
static CR_STATUS ThreadStatus;
static CR_SM8450_GPU_VIEW_DESCRIPTOR Views[]={
 {1,1,0x03d00000,0x40000},{2,1,0x03d61000,0x800},{3,1,0x03d6a000,0x26000},
 {4,1,0x03d9a000,0x5000},{5,1,0x03d50000,0x10000},{6,1,0x0b290000,0x10000},
 {7,2,0x03d90000,0xa000}};
static CR_STATUS Call(MODEL *D,UINT64 Epoch){
  CHECK(Epoch==77);++D->Calls;
  if(D->Calls==D->PauseAt){pthread_mutex_lock(&PauseLock);Paused=1;pthread_cond_broadcast(&PauseCond);
    while(!Resume)pthread_cond_wait(&PauseCond,&PauseLock);pthread_mutex_unlock(&PauseLock);}
  if(D->Calls==D->SealAt)D->SealResult=CrAdrenoStopSeal(D->Owner);
  if(D->Calls==D->BusyAt){CHECK(CrAdrenoStopFence(D->Owner)==CR_BUSY);
    CHECK(CrAdrenoStopInvalidate(D->Owner)==CR_BUSY);}
  if(D->Calls==D->MutateAt)D->Reg[CrAdrenoStopGxStatus]^=1;
  if(D->Calls==D->FailAt)return CR_DEVICE_ERROR;
  if(D->Calls==D->WarnAt)return (CR_STATUS)1;
  return CR_SUCCESS;
}
static CR_STATUS Access(MODEL *D,CR_ADRENO_STOP_REGISTER R,UINT32 Required){
  CR_ADRENO_STOP_REGISTER_INFO I;CR_SM8450_GPU_REGISTER_SPAN Span;
  CHECK(CrAdrenoSm8450StopDescribe(R,&I)==CR_SUCCESS);
  CHECK(I.Access&Required);CHECK(CrAdrenoSm8450StopResolve(&D->Layout,R,&Span)==CR_SUCCESS);
  CHECK(Span.Relation==CR_SM8450_GPU_RELATION_DEDICATED && Span.Bytes==4 && Span.View!=7);
  CHECK(Span.PhysicalAddress==ReferencePhysical[R]);
  if((I.Access&CR_ADRENO_STOP_REG_GX) && (D->Reg[CrAdrenoStopGxStatus]&3)){
    ++D->GxDenied;return CR_NOT_READY;
  }
  return CR_SUCCESS;
}
static CR_STATUS Read(void *P,UINT64 E,CR_ADRENO_STOP_REGISTER R,UINT32 *V){
  MODEL *D=P;CR_STATUS S=Call(D,E);if(S!=CR_SUCCESS)return S;
  S=Access(D,R,CR_ADRENO_STOP_REG_READ);if(S!=CR_SUCCESS)return S;++D->Reads;
  if(R>=CrAdrenoStopTcs0)D->SeenTcs|=1U<<(R-CrAdrenoStopTcs0);
  if(D->EmptyRead)return CR_SUCCESS;
  *V=D->Reg[R];
  if(R==D->Stuck){
    if(R==CrAdrenoStopRsccBusy)*V=1;else *V=0;
  }
  if(R==CrAdrenoStopCxAck && D->BadAck)*V=2; /* stale ARB ACK during client1 */
  return CR_SUCCESS;
}
static CR_STATUS Write(void *P,UINT64 E,CR_ADRENO_STOP_REGISTER R,UINT32 V){
  MODEL *D=P;CR_STATUS S=Call(D,E);if(S!=CR_SUCCESS)return S;
  S=Access(D,R,CR_ADRENO_STOP_REG_WRITE);if(S!=CR_SUCCESS)return S;
  ++D->Writes;D->Written|=BIT64(R);
  if(R==CrAdrenoStopCxHalt){CHECK(V==1||V==2);CHECK(!(D->Reg[R]==2 && V==1));
    if(V==1)++D->ClientWrites;else ++D->ArbWrites;}
  if(R==CrAdrenoStopCm3Reset||R==CrAdrenoStopGpuReset||R==CrAdrenoStopGxHalt)CHECK(V==1);
  if(R==CrAdrenoStopAhbClear){CHECK(V==7);++D->ClearCommands;}
  if(R!=D->Drop && R!=CrAdrenoStopAhbClear){
    D->Reg[R]=V;
    if(R==CrAdrenoStopGxHalt)D->Reg[CrAdrenoStopGxAck]=V;
    if(R==CrAdrenoStopCxHalt)D->Reg[CrAdrenoStopCxAck]=V;
  }
  if(D->Calls==D->AfterWriteFailAt)return CR_DEVICE_ERROR;
  return CR_SUCCESS;
}
static CR_STATUS Barrier(void *P,UINT64 E){MODEL *D=P;CR_STATUS S=Call(D,E);++D->Barriers;return S;}
static CR_STATUS Delay(void *P,UINT64 E,UINT32 Us){MODEL *D=P;CR_STATUS S=Call(D,E);
  CHECK(Us==100);++D->Delays;D->DelayUs+=Us;if(D->Reg[CrAdrenoStopGpuReset]==1)D->ResetDelayUs+=Us;return S;}
static void Init(UINT32 Gx){
  memset(&M,0,sizeof(M));memset(&C,0,sizeof(C));memset(&Io,0,sizeof(Io));
  M.Owner=&C;M.Stuck=M.Drop=0xffffffffU;M.Reg[CrAdrenoStopGxStatus]=Gx;
  M.Reg[CrAdrenoStopGpuIrqMask]=0xffffffffU;M.Reg[CrAdrenoStopKeepalive]=1;
  M.Reg[CrAdrenoStopAhbFence]=9;
  for(UINT32 I=CrAdrenoStopTcs0;I<CrAdrenoStopRegisterCount;++I)M.Reg[I]=1;
  CHECK(CrSm8450GpuResourcesBuild(Views,7,&M.Layout)==CR_SUCCESS);
  Io=(CR_ADRENO_STOP_IO){&M,sizeof(M),77,Read,Write,Barrier,Delay};
  CHECK(CrAdrenoStopInit(&C,&Io,77)==CR_SUCCESS);CHECK(M.Calls==0);
}
static void CheckFail(CR_STATUS Want){
  CR_ADRENO_STOP_OBSERVATION O,Saved;memset(&O,0x91,sizeof(O));Saved=O;
  CHECK(CrAdrenoStopCheck(&C,&O)==Want);CHECK(memcmp(&O,&Saved,sizeof(O))==0);
}
static void Valid(void){
  for(UINT32 Gx=0;Gx<4;++Gx){Init(Gx);CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);
    CHECK(C.HeldGeneration==1 && C.Phase==CrAdrenoStopPhaseHeld && C.Gate==0);
    CHECK(M.Reg[CrAdrenoStopCxHalt]==2 && M.Reg[CrAdrenoStopCxAck]==2);
    CHECK(M.Reg[CrAdrenoStopCm3Reset]==1 && M.SeenTcs==1023 && M.ClearCommands==1);
    CHECK(M.ClientWrites==1 && M.ArbWrites==1 && M.GxDenied==0);
    CHECK(Gx?M.ResetDelayUs==0:M.ResetDelayUs>=100);
    if(Gx)CHECK((M.Written&(BIT64(CrAdrenoStopGpuIrqMask)|BIT64(CrAdrenoStopGpuReset)|BIT64(CrAdrenoStopGxHalt)))==0);
    CR_ADRENO_STOP_OBSERVATION O;UINT32 Writes=M.Writes;
    CHECK(CrAdrenoStopCheck(&C,&O)==CR_SUCCESS);CHECK(M.Writes==Writes);
    CHECK(O.Version==1 && O.Size==160 && O.Policy==1 && O.GxState==Gx && O.StartIncarnation==77 && O.Generation==1);
    CHECK(O.AttemptedWrites==M.Written && O.AcknowledgedWrites==M.Written);
    CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS && C.Generation==2 && M.ClientWrites==1);
    CHECK(CrAdrenoStopInvalidate(&C)==CR_SUCCESS);CheckFail(CR_NOT_READY);
    M.Reg[CrAdrenoStopGxStatus]=0;CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);
    CHECK(CrAdrenoStopSeal(&C)==CR_SUCCESS);UINT32 Calls=M.Calls;
    CHECK(CrAdrenoStopFence(&C)==CR_ABORT);CHECK(CrAdrenoStopInvalidate(&C)==CR_ABORT);
    CheckFail(CR_ABORT);CHECK(CrAdrenoStopSeal(&C)==CR_SUCCESS && M.Calls==Calls);
    CHECK(CrAdrenoStopInit(&C,&Io,77)==CR_ALREADY_STARTED);
  }
}
static void EveryFailure(void){
  Init(0);CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);UINT32 Calls=M.Calls;
  for(UINT32 I=1;I<=Calls;++I){
    Init(0);M.FailAt=I;CHECK(CrAdrenoStopFence(&C)==CR_DEVICE_ERROR);
    CHECK(M.Calls==I && C.HeldGeneration==0 && C.FirstFailure==CR_DEVICE_ERROR && C.Gate==0);
    UINT32 Phase=C.FirstFailurePhase;UINT64 Attempted=C.Observation.AttemptedWrites;
    M.FailAt=0;CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);
    CHECK(C.FirstFailurePhase==Phase && C.FirstFailure==CR_DEVICE_ERROR);
    CHECK((C.Observation.AttemptedWrites&Attempted)==Attempted);
    Init(0);M.WarnAt=I;CHECK(CrAdrenoStopFence(&C)==CR_DEVICE_ERROR);
    CHECK(M.Calls==I && C.FirstRawStatus==(CR_STATUS)1 && C.HeldGeneration==0);
    Init(0);M.SealAt=I;CHECK(CrAdrenoStopFence(&C)==CR_ABORT);
    CHECK(M.SealResult==CR_BUSY && M.Calls==I && C.HeldGeneration==0);
    CHECK(CrAdrenoStopSeal(&C)==CR_SUCCESS && C.Gate==CR_ADRENO_STOP_CLOSED);
    Init(0);M.BusyAt=I;CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);
  }
  /* A write may take effect and return error; all exact write positions retry. */
  for(UINT32 I=1;I<=Calls;++I){Init(0);M.AfterWriteFailAt=I;CR_STATUS S=CrAdrenoStopFence(&C);
    if(S==CR_SUCCESS)continue;CHECK(S==CR_DEVICE_ERROR && C.HeldGeneration==0);
    M.AfterWriteFailAt=0;CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);}
}
static void TimeoutsAndDrops(void){
  for(UINT32 R=CrAdrenoStopTcs0;R<CrAdrenoStopRegisterCount;++R){
    Init(0);M.Stuck=R;CHECK(CrAdrenoStopFence(&C)==CR_TIMEOUT);
    CHECK(M.DelayUs==1000 && M.Reg[CrAdrenoStopCm3Reset]==0 && M.SeenTcs==((1U<<(R-CrAdrenoStopTcs0+1))-1));
  }
  Init(0);M.Stuck=CrAdrenoStopRsccBusy;CHECK(CrAdrenoStopFence(&C)==CR_TIMEOUT && M.DelayUs==10000);
  Init(0);M.Stuck=CrAdrenoStopGxAck;CHECK(CrAdrenoStopFence(&C)==CR_TIMEOUT && M.DelayUs==100000);
  Init(0);M.Stuck=CrAdrenoStopCxAck;CHECK(CrAdrenoStopFence(&C)==CR_TIMEOUT && M.DelayUs==100000);
  Init(0);M.BadAck=TRUE;CHECK(CrAdrenoStopFence(&C)==CR_TIMEOUT && M.ClientWrites==1 && M.ArbWrites==0);
  static const UINT32 Drops[]={CrAdrenoStopGpuIrqMask,CrAdrenoStopGmuIrqMask,CrAdrenoStopAoIrqMask,
    CrAdrenoStopKeepalive,CrAdrenoStopQactive,CrAdrenoStopAhbFence,CrAdrenoStopCm3Reset,
    CrAdrenoStopGxHalt,CrAdrenoStopCxHalt,CrAdrenoStopGpuReset};
  for(UINT32 I=0;I<sizeof(Drops)/sizeof(Drops[0]);++I){
    Init(0);M.Drop=Drops[I];CHECK(CrAdrenoStopFence(&C)==CR_DEVICE_ERROR);
    CHECK(C.HeldGeneration==0 && (C.Observation.AttemptedWrites&BIT64(Drops[I])));
  }
  Init(0);M.EmptyRead=TRUE;CHECK(CrAdrenoStopFence(&C)!=CR_SUCCESS && C.HeldGeneration==0);
  Init(0);M.Reg[CrAdrenoStopCxHalt]=3;CHECK(CrAdrenoStopFence(&C)==CR_UNSUPPORTED && M.ArbWrites==0);
}
static void LostConditions(void){
  static const UINT32 Regs[]={CrAdrenoStopGxStatus,CrAdrenoStopGpuIrqMask,CrAdrenoStopGmuIrqMask,
    CrAdrenoStopAoIrqMask,CrAdrenoStopKeepalive,CrAdrenoStopQactive,CrAdrenoStopAhbFence,
    CrAdrenoStopCm3Reset,CrAdrenoStopGxHalt,CrAdrenoStopGxAck,CrAdrenoStopCxHalt,
    CrAdrenoStopCxAck,CrAdrenoStopGpuReset,CrAdrenoStopRsccBusy};
  for(UINT32 I=0;I<sizeof(Regs)/sizeof(Regs[0]);++I){
    Init(0);CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);UINT32 R=Regs[I];
    M.Reg[R]^=(R==CrAdrenoStopCxAck)?2:1;UINT32 W=M.Writes;CheckFail(CR_DEVICE_ERROR);
    CHECK(M.Writes==W && C.HeldGeneration==0);
  }
  for(UINT32 R=CrAdrenoStopTcs0;R<CrAdrenoStopRegisterCount;++R){
    Init(0);CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);M.Reg[R]=0;CheckFail(CR_DEVICE_ERROR);}
  Init(0);CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);M.Reg[CrAdrenoStopCxBusy]=1U<<23;
  CR_ADRENO_STOP_OBSERVATION O;CHECK(CrAdrenoStopCheck(&C,&O)==CR_SUCCESS);
  CHECK(O.Values[CrAdrenoStopCxBusy]==1U<<23); /* not graceful-idle evidence */
  Init(0);M.MutateAt=20;CHECK(CrAdrenoStopFence(&C)!=CR_SUCCESS && C.HeldGeneration==0);
}
static void IdentityAndRanges(void){
  Init(0);CR_ADRENO_STOP_CONTEXT Copy=C;CHECK(CrAdrenoStopFence(&Copy)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoStopSeal(&Copy)==CR_INVALID_PARAMETER && M.Calls==0);
  C.StartIncarnation=78;CHECK(CrAdrenoStopFence(&C)==CR_INVALID_PARAMETER);C.StartIncarnation=77;
  Io.StartIncarnation=78;CHECK(CrAdrenoStopFence(&C)==CR_INVALID_PARAMETER);Io.StartIncarnation=77;
  Io.Read=NULL;CHECK(CrAdrenoStopFence(&C)==CR_INVALID_PARAMETER);Io.Read=Read;
  CHECK(CrAdrenoStopFence(&C)==CR_SUCCESS);UINT32 Calls=M.Calls;
  CHECK(CrAdrenoStopCheck(&C,(void *)&C)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoStopCheck(&C,(void *)&M)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoStopCheck(&C,(void *)&Io)==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoStopCheck(&C,(void *)(uintptr_t)(UINTPTR_MAX-7))==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoStopFence((void *)(uintptr_t)(UINTPTR_MAX-7))==CR_INVALID_PARAMETER);
  CHECK(CrAdrenoStopCheck(&C,NULL)==CR_INVALID_PARAMETER && M.Calls==Calls);
  CR_ADRENO_STOP_CONTEXT N={0};CHECK(CrAdrenoStopInit(&N,&Io,0)==CR_INVALID_PARAMETER);
  Io.ContextBytes=0;CHECK(CrAdrenoStopInit(&N,&Io,77)==CR_INVALID_PARAMETER);Io.ContextBytes=sizeof(M);
  Io.Context=&N;Io.ContextBytes=sizeof(N);CHECK(CrAdrenoStopInit(&N,&Io,77)==CR_INVALID_PARAMETER);
  Init(0);C.Generation=~(UINT64)0;CHECK(CrAdrenoStopFence(&C)==CR_OUT_OF_RESOURCES && M.Calls==0);
  Init(0);for(UINT32 R=0;R<CrAdrenoStopRegisterCount;++R){
    CR_ADRENO_STOP_REGISTER_INFO Info;CR_SM8450_GPU_REGISTER_SPAN S;
    CHECK(CrAdrenoSm8450StopDescribe((CR_ADRENO_STOP_REGISTER)R,&Info)==CR_SUCCESS);
    CHECK(CrAdrenoSm8450StopResolve(&M.Layout,(CR_ADRENO_STOP_REGISTER)R,&S)==CR_SUCCESS);
    CHECK(S.PhysicalAddress==ReferencePhysical[R]);}
  CR_ADRENO_STOP_REGISTER_INFO Info,Saved;memset(&Info,0x39,sizeof(Info));Saved=Info;
  CHECK(CrAdrenoSm8450StopDescribe((CR_ADRENO_STOP_REGISTER)0xffffffffU,&Info)==CR_INVALID_PARAMETER);
  CHECK(memcmp(&Info,&Saved,sizeof(Info))==0);
  CR_SM8450_GPU_REGISTER_SPAN S,Old;memset(&S,0x39,sizeof(S));Old=S;M.Layout.Views[0].Base++;
  CHECK(CrAdrenoSm8450StopResolve(&M.Layout,CrAdrenoStopGpuReset,&S)==CR_COMPROMISED_DATA);
  CHECK(memcmp(&S,&Old,sizeof(S))==0);
}
static void *Worker(void *Unused){(void)Unused;ThreadStatus=CrAdrenoStopFence(&C);return NULL;}
static void ConcurrentSeal(void){
  Init(0);M.PauseAt=20;Paused=Resume=0;pthread_t T;CHECK(pthread_create(&T,NULL,Worker,NULL)==0);
  pthread_mutex_lock(&PauseLock);while(!Paused)pthread_cond_wait(&PauseCond,&PauseLock);
  CHECK(CrAdrenoStopSeal(&C)==CR_BUSY);Resume=1;pthread_cond_broadcast(&PauseCond);pthread_mutex_unlock(&PauseLock);
  CHECK(pthread_join(T,NULL)==0);CHECK(ThreadStatus==CR_ABORT && M.Calls==20);
  CHECK(CrAdrenoStopSeal(&C)==CR_SUCCESS && C.Gate==CR_ADRENO_STOP_CLOSED);
}
int main(void){Valid();EveryFailure();TimeoutsAndDrops();LostConditions();IdentityAndRanges();ConcurrentSeal();
  printf("Actual SM8450 stop/target/resolver: %u checks PASS\n",Checks);return 0;}
