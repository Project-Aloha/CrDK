/* Checked retained reset/AXI fence; GPL-2.0-only.
 * Sequence sources: Android Gen7 pwrctrl_suspend (e93f69dc) and Linux
 * a6xx_gmu_force_off/bus_clear_pending_transactions (a53261b6).
 * Copyright source attribution: Linux Foundation2017-2021;
 * Qualcomm Innovation Center2022-2024. New checked failure/lifetime logic.
 */
#include <Library/AdrenoStopCrLib.h>
#include <oskal/cr_atomic.h>
#define BIT64(N) (((UINT64)1)<<(N))
#define GMU_IRQ_BITS 0xffff8007U /* Android HFI/OOB + mainline message/fault */
#define AO_IRQ_BITS 0x3fU       /* six defined source AO interrupt bits */
#define POLL_US 100U
#define TCS_US 1000U           /* Android suspend's1ms per TCS */
#define HALT_US 100000U        /* Android _do_gbif_halt's100ms */
#define RSC_US 10000U          /* source sequence-busy10ms bound */

STATIC BOOLEAN Span(CONST VOID *P,UINTN N,UINTN Align) {
  return P!=NULL && (UINTN)P%Align==0 && N!=0 && (UINTN)P<=~(UINTN)0-N;
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
STATIC BOOLEAN Identity(CR_ADRENO_STOP_CONTEXT *C) {
  if(!Span(C,sizeof(*C),8) || C->Self!=C || !Span(C->OriginalIo,sizeof(*C->OriginalIo),8))return FALSE;
  CONST CR_ADRENO_STOP_IO *I=C->OriginalIo;
  return C->StartIncarnation!=0 && C->StartIncarnation==C->Io.StartIncarnation &&
    I->Context==C->Io.Context && I->ContextBytes==C->Io.ContextBytes &&
    I->StartIncarnation==C->Io.StartIncarnation && I->Read==C->Io.Read &&
    I->Write==C->Io.Write && I->Barrier==C->Io.Barrier && I->DelayUs==C->Io.DelayUs;
}
STATIC CR_STATUS Live(CR_ADRENO_STOP_CONTEXT *C) {
  if(!Identity(C))return CR_INVALID_PARAMETER;
  return (CrAtomicLoad32(&C->Gate)&CR_ADRENO_STOP_CLOSED)?CR_ABORT:CR_SUCCESS;
}
STATIC CR_STATUS Enter(CR_ADRENO_STOP_CONTEXT *C) {
  CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;
  UINT32 Old=Compare(&C->Gate,0,CR_ADRENO_STOP_ACTIVE);
  return Old==0?CR_SUCCESS:(Old&CR_ADRENO_STOP_CLOSED)?CR_ABORT:CR_BUSY;
}
STATIC CR_STATUS Fail(CR_ADRENO_STOP_CONTEXT *C,CR_STATUS S) {
  C->HeldGeneration=0;C->LastStatus=S;
  if(C->FirstFailure==CR_SUCCESS){C->FirstFailure=S;C->FirstFailurePhase=C->Phase;}
  return S;
}
STATIC CR_STATUS Leave(CR_ADRENO_STOP_CONTEXT *C,CR_STATUS S) {
  if(S==CR_SUCCESS){S=Live(C);if(S!=CR_SUCCESS)Fail(C,S);else C->LastStatus=CR_SUCCESS;}
  else Fail(C,S);
  CrAtomicAnd32(&C->Gate,~CR_ADRENO_STOP_ACTIVE);return S;
}
STATIC CR_STATUS Result(CR_ADRENO_STOP_CONTEXT *C,CR_STATUS S) {
  if(S!=CR_SUCCESS){
    if(C->FirstFailure==CR_SUCCESS)C->FirstRawStatus=S;
    return Fail(C,CR_ERROR(S)?S:CR_DEVICE_ERROR);
  }
  return Live(C);
}
STATIC CR_STATUS Read(CR_ADRENO_STOP_CONTEXT *C,CR_ADRENO_STOP_REGISTER R,UINT32 *Out) {
  CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;
  UINT32 V=0xbad0bad0U;
  S=C->Io.Read(C->Io.Context,C->StartIncarnation,R,&V);
  if(S==CR_SUCCESS){C->Observation.Values[R]=V;C->Observation.ReadMask|=BIT64(R);}
  S=Result(C,S);if(S==CR_SUCCESS)*Out=V;return S;
}
STATIC CR_STATUS Write(CR_ADRENO_STOP_CONTEXT *C,CR_ADRENO_STOP_REGISTER R,UINT32 Value) {
  CR_STATUS S=Live(C);if(S!=CR_SUCCESS)return S;
  C->Observation.AttemptedWrites|=BIT64(R);
  S=C->Io.Write(C->Io.Context,C->StartIncarnation,R,Value);
  if(S==CR_SUCCESS)C->Observation.AcknowledgedWrites|=BIT64(R);
  return Result(C,S);
}
STATIC CR_STATUS Barrier(CR_ADRENO_STOP_CONTEXT *C) {
  CR_STATUS S=Live(C);return S==CR_SUCCESS?Result(C,C->Io.Barrier(C->Io.Context,C->StartIncarnation)):S;
}
STATIC CR_STATUS Delay(CR_ADRENO_STOP_CONTEXT *C,UINT32 Us) {
  CR_STATUS S=Live(C);return S==CR_SUCCESS?Result(C,C->Io.DelayUs(C->Io.Context,C->StartIncarnation,Us)):S;
}
STATIC CR_STATUS Equal(CR_ADRENO_STOP_CONTEXT *C,CR_ADRENO_STOP_REGISTER R,UINT32 Mask,UINT32 Want) {
  UINT32 V;CR_STATUS S=Read(C,R,&V);return S!=CR_SUCCESS?S:(V&Mask)==Want?CR_SUCCESS:CR_DEVICE_ERROR;
}
STATIC CR_STATUS Set(CR_ADRENO_STOP_CONTEXT *C,CR_ADRENO_STOP_REGISTER R,UINT32 Value,UINT32 Mask) {
  CR_STATUS S=Write(C,R,Value);if(S!=CR_SUCCESS)return S;
  S=Barrier(C);return S!=CR_SUCCESS?S:Equal(C,R,Mask,Value&Mask);
}
STATIC CR_STATUS Poll(CR_ADRENO_STOP_CONTEXT *C,CR_ADRENO_STOP_REGISTER R,UINT32 Mask,UINT32 Want,UINT32 Limit) {
  for(UINT32 Us=0;;Us+=POLL_US){
    UINT32 V;CR_STATUS S=Read(C,R,&V);if(S!=CR_SUCCESS)return S;
    if((V&Mask)==Want)return CR_SUCCESS;
    if(Us>=Limit)return CR_TIMEOUT;
    S=Delay(C,POLL_US);if(S!=CR_SUCCESS)return S;
  }
}
STATIC CR_STATUS Votes(CR_ADRENO_STOP_CONTEXT *C,BOOLEAN Wait) {
  for(UINT32 I=0;I<CR_ADRENO_STOP_TCS_COUNT;++I){
    CR_ADRENO_STOP_REGISTER R=(CR_ADRENO_STOP_REGISTER)(CrAdrenoStopTcs0+I);
    CR_STATUS S=Wait?Poll(C,R,1,1,TCS_US):Equal(C,R,1,1);if(S!=CR_SUCCESS)return S;
  }
  return Wait?Poll(C,CrAdrenoStopRsccBusy,0xffffffffU,0,RSC_US):Equal(C,CrAdrenoStopRsccBusy,0xffffffffU,0);
}
STATIC CR_STATUS Gx(CR_ADRENO_STOP_CONTEXT *C) {
  return Equal(C,CrAdrenoStopGxStatus,3,C->Observation.GxState);
}
#define TRY(X) do{CR_STATUS Step=(X);if(Step!=CR_SUCCESS)return Step;}while(0)
STATIC CR_STATUS Verify(CR_ADRENO_STOP_CONTEXT *C) {
  TRY(Gx(C));TRY(Equal(C,CrAdrenoStopCm3Reset,0xffffffffU,1));
  TRY(Equal(C,CrAdrenoStopCxHalt,0xffffffffU,2));TRY(Equal(C,CrAdrenoStopCxAck,2,2));
  TRY(Equal(C,CrAdrenoStopGmuIrqMask,GMU_IRQ_BITS,GMU_IRQ_BITS));
  TRY(Equal(C,CrAdrenoStopAoIrqMask,AO_IRQ_BITS,AO_IRQ_BITS));
  TRY(Equal(C,CrAdrenoStopKeepalive,0xffffffffU,0));TRY(Equal(C,CrAdrenoStopQactive,1,1));
  TRY(Equal(C,CrAdrenoStopAhbFence,0xffffffffU,0));
  if(C->Observation.GxState==0){
    TRY(Gx(C));TRY(Equal(C,CrAdrenoStopGpuIrqMask,0xffffffffU,0));
    TRY(Gx(C));TRY(Equal(C,CrAdrenoStopGxHalt,0xffffffffU,1));
    TRY(Gx(C));TRY(Equal(C,CrAdrenoStopGxAck,1,1));
    TRY(Gx(C));TRY(Equal(C,CrAdrenoStopGpuReset,0xffffffffU,1));
  }
  TRY(Votes(C,FALSE));UINT32 Busy;TRY(Read(C,CrAdrenoStopCxBusy,&Busy));
  TRY(Gx(C));return CR_SUCCESS;
}
STATIC CR_STATUS Sequence(CR_ADRENO_STOP_CONTEXT *C) {
  C->Phase=CrAdrenoStopPhaseMasks;UINT32 Value;
  TRY(Read(C,CrAdrenoStopGxStatus,&Value));
  /* After any mutation, a retry cannot reinterpret changed power as absence. */
  if(C->BranchPinned && (Value&3)!=C->Observation.GxState)return CR_DEVICE_ERROR;
  C->Observation.GxState=Value&3;C->BranchPinned=1;
  if(C->Observation.GxState==0){TRY(Gx(C));TRY(Set(C,CrAdrenoStopGpuIrqMask,0,0xffffffffU));}
  TRY(Set(C,CrAdrenoStopGmuIrqMask,0xffffffffU,GMU_IRQ_BITS));
  TRY(Set(C,CrAdrenoStopAoIrqMask,0xffffffffU,AO_IRQ_BITS));
  TRY(Set(C,CrAdrenoStopKeepalive,0,0xffffffffU));TRY(Set(C,CrAdrenoStopQactive,1,1));
  C->Phase=CrAdrenoStopPhaseVotes;TRY(Votes(C,TRUE));
  C->Phase=CrAdrenoStopPhaseCm3;TRY(Write(C,CrAdrenoStopAhbClear,7));
  TRY(Set(C,CrAdrenoStopAhbFence,0,0xffffffffU));TRY(Set(C,CrAdrenoStopCm3Reset,1,0xffffffffU));
  C->Phase=CrAdrenoStopPhaseGx;TRY(Gx(C));
  if(C->Observation.GxState==0){
    TRY(Set(C,CrAdrenoStopGxHalt,1,0xffffffffU));TRY(Poll(C,CrAdrenoStopGxAck,1,1,HALT_US));
  }
  /* An earlier ARB write may have taken effect even when its callback failed.
   * Read control first. Never weaken existing ARB2 back to CLIENT1 on retry. */
  C->Phase=CrAdrenoStopPhaseClient;TRY(Read(C,CrAdrenoStopCxHalt,&Value));
  if(Value!=0 && Value!=1 && Value!=2)return CR_UNSUPPORTED;
  if(Value!=2){TRY(Set(C,CrAdrenoStopCxHalt,1,0xffffffffU));TRY(Poll(C,CrAdrenoStopCxAck,1,1,HALT_US));}
  C->Phase=CrAdrenoStopPhaseArb;TRY(Set(C,CrAdrenoStopCxHalt,2,0xffffffffU));
  TRY(Poll(C,CrAdrenoStopCxAck,2,2,HALT_US));
  C->Phase=CrAdrenoStopPhaseGpuReset;TRY(Gx(C));
  if(C->Observation.GxState==0){TRY(Set(C,CrAdrenoStopGpuReset,1,0xffffffffU));TRY(Delay(C,100));}
  C->Phase=CrAdrenoStopPhaseVerify;TRY(Verify(C));C->Phase=CrAdrenoStopPhaseHeld;return CR_SUCCESS;
}
#undef TRY
CR_STATUS CrAdrenoStopInit(CR_ADRENO_STOP_CONTEXT *C,CONST CR_ADRENO_STOP_IO *Io,UINT64 Epoch) {
  if(!Span(C,sizeof(*C),8)||!Span(Io,sizeof(*Io),8)||Overlap(C,sizeof(*C),Io,sizeof(*Io)))return CR_INVALID_PARAMETER;
  if(C->Self || CrAtomicLoad32(&C->Gate))return CR_ALREADY_STARTED;
  if(!Span(Io->Context,Io->ContextBytes,1)||Io->StartIncarnation!=Epoch||!Epoch||
      !Io->Read||!Io->Write||!Io->Barrier||!Io->DelayUs||
      Overlap(C,sizeof(*C),Io->Context,Io->ContextBytes)||
      Overlap(Io,sizeof(*Io),Io->Context,Io->ContextBytes))return CR_INVALID_PARAMETER;
  CR_ADRENO_STOP_CONTEXT New={0};New.Self=C;New.OriginalIo=Io;New.Io=*Io;New.StartIncarnation=Epoch;
  New.Observation.Version=CR_ADRENO_STOP_VERSION;New.Observation.Size=sizeof(New.Observation);
  New.Observation.Policy=CR_ADRENO_STOP_POLICY_SM8450_ANDROID;New.Observation.StartIncarnation=Epoch;
  *C=New;return CR_SUCCESS;
}
CR_STATUS CrAdrenoStopFence(CR_ADRENO_STOP_CONTEXT *C) {
  CR_STATUS S=Enter(C);if(S!=CR_SUCCESS)return S;
  C->HeldGeneration=0;
  if(C->Generation==~(UINT64)0)return Leave(C,CR_OUT_OF_RESOURCES);
  C->Observation.Generation=++C->Generation;C->Observation.ReadMask=0;
  S=Sequence(C);if(S==CR_SUCCESS)C->HeldGeneration=C->Generation;
  return Leave(C,S);
}
CR_STATUS CrAdrenoStopCheck(CR_ADRENO_STOP_CONTEXT *C,CR_ADRENO_STOP_OBSERVATION *Out) {
  if(!Identity(C)||!Span(Out,sizeof(*Out),8)||Overlap(C,sizeof(*C),Out,sizeof(*Out))||
      Overlap(C->OriginalIo,sizeof(*C->OriginalIo),Out,sizeof(*Out))||
      Overlap(C->Io.Context,C->Io.ContextBytes,Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  CR_STATUS S=Enter(C);if(S!=CR_SUCCESS)return S;
  if(!C->HeldGeneration || C->HeldGeneration!=C->Generation)return Leave(C,CR_NOT_READY);
  C->Phase=CrAdrenoStopPhaseVerify;C->Observation.ReadMask=0;S=Verify(C);
  if(S==CR_SUCCESS)S=Live(C);
  if(S==CR_SUCCESS){C->Phase=CrAdrenoStopPhaseHeld;*Out=C->Observation;}
  /* Publication linearizes at the final Live check; concurrent Seal may close
   * afterward. It retains this active call and cannot retire its storage. */
  if(S!=CR_SUCCESS)Fail(C,S);else C->LastStatus=CR_SUCCESS;
  CrAtomicAnd32(&C->Gate,~CR_ADRENO_STOP_ACTIVE);return S;
}
CR_STATUS CrAdrenoStopInvalidate(CR_ADRENO_STOP_CONTEXT *C) {
  CR_STATUS S=Enter(C);if(S!=CR_SUCCESS)return S;
  C->HeldGeneration=0;
  if(C->Generation==~(UINT64)0)return Leave(C,CR_OUT_OF_RESOURCES);
  ++C->Generation;C->BranchPinned=0;C->Phase=CrAdrenoStopPhaseNone;return Leave(C,CR_SUCCESS);
}
CR_STATUS CrAdrenoStopSeal(CR_ADRENO_STOP_CONTEXT *C) {
  if(!Span(C,sizeof(*C),8)||C->Self!=C)return CR_INVALID_PARAMETER;
  UINT32 Old=CrAtomicOr32(&C->Gate,CR_ADRENO_STOP_CLOSED);
  return (Old&CR_ADRENO_STOP_ACTIVE)?CR_BUSY:CR_SUCCESS;
}
