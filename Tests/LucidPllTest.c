/* Actual direct Lucid owner versus original Android operations and failures.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#endif
#include <Library/LucidPllCrLib.h>
#ifdef _KERNEL_MODE
STATIC KIRQL TestIrql;
KIRQL KeGetCurrentIrql(VOID){return TestIrql;}
#endif
#ifndef _KERNEL_MODE
#include <Library/BaseMemoryLib.h>
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V) { return memset(P,V,N); }
#endif
typedef struct {UINT32 Offset,Value;} OP;
typedef struct {
  UINT32 Reg[0x44/4],Calls,Fail,Writes,DelayUs,Drop,TraceCount,Runs;
  UINT32 PendingOffset,PendingValue;
  BOOLEAN Commit,Posted,Pending,Quiet,Retired,Cal,Run,LockNever,LockStale;
  BOOLEAN RetainOffToken,FailCalOff,Reactivate,ForeignOwner,FailRunOnToken,Warning;
  OP Trace[128];
} FIXTURE;
STATIC FIXTURE F;
STATIC OP Oracle[128];STATIC UINT32 OracleCount;
extern VOID AndroidLucidReference(VOID);
void OracleLucidWrite(UINT32 O,UINT32 V)
{ assert(OracleCount<128);Oracle[OracleCount++]=(OP){O,V}; }
STATIC CR_STATUS Tick(VOID)
{return ++F.Calls==F.Fail?(F.Warning?(CR_STATUS)1:CR_DEVICE_ERROR):CR_SUCCESS;}
STATIC CR_STATUS Ready(VOID *C,UINT32 R)
{
  assert(C==&F&&(R&3)==3);CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  if(!F.Retired || ((R&16)&&!F.Quiet) || ((R&4)&&!F.Cal) || ((R&8)&&!F.Run&&!F.Cal))
    return CR_NOT_READY;
  return CR_SUCCESS;
}
STATIC CR_STATUS Read(VOID *C,UINT32 O,UINT32 *V)
{
  assert(C==&F&&!(O&3)&&O<sizeof(F.Reg)&&(F.Cal||F.Run));
  assert(O!=0x30); /* no ambiguous STATUS calibration shortcut */
  CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  *V=F.Reg[O/4];
  if(O==0){*V&=~(1U<<31);if(F.LockStale||(!F.LockNever&&(F.Reg[0x38/4]&1)))*V|=1U<<31;}
  return CR_SUCCESS;
}
STATIC VOID Commit(UINT32 O,UINT32 V)
{
  F.Reg[O/4]=O?V:V&~(1U<<31);
  if(O==0x38&&V==1)++F.Runs;
  if(F.Reactivate&&O==0x38&&!V)F.Reg[0]|=1;
}
STATIC CR_STATUS Write(VOID *C,UINT32 O,UINT32 V)
{
  assert(C==&F&&!(O&3)&&O<sizeof(F.Reg)&&(F.Cal||F.Run)&&F.Quiet&&F.Retired);
  CR_STATUS S=Tick();++F.Writes;if(S!=CR_SUCCESS&&!F.Commit)return S;
  assert(O!=0x30&&F.TraceCount<128);F.Trace[F.TraceCount++]=(OP){O,V};
  if(F.Drop==O+1)return S;
  if(F.Posted){F.PendingOffset=O;F.PendingValue=V;F.Pending=TRUE;}
  else Commit(O,V);
  return S;
}
STATIC CR_STATUS Fence(VOID *C)
{
  assert(C==&F);CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  if(F.Pending){Commit(F.PendingOffset,F.PendingValue);F.Pending=FALSE;}
  return CR_SUCCESS;
}
STATIC CR_STATUS Delay(VOID *C,UINT32 Us)
{assert(C==&F);F.DelayUs+=Us;return Tick();}
STATIC CR_STATUS Vote(VOID *C,UINT32 Level,BOOLEAN On,BOOLEAN *Owned)
{
  assert(C==&F&&(Level==192||Level==64)&&F.Retired&&F.Quiet);
  CR_STATUS S=Tick();
  /* A failed provider can retain a token before raising voltage. The
   * separately applied calibration vote still powers cleanup at this point. */
  if(Level==64&&On&&F.FailRunOnToken){*Owned=TRUE;return CR_DEVICE_ERROR;}
  if(Level==192&&!On&&F.FailCalOff)S=CR_DEVICE_ERROR;
  if(S!=CR_SUCCESS&&!F.Commit)return S;
  if(!On&&!F.ForeignOwner){ /* no owned output may depend on withdrawn vote */
    assert(!(F.Reg[0]&1)&&!(F.Reg[0x0c/4]&7)&&!(F.Reg[0x38/4]&1));
  }
  if(Level==192)F.Cal=On;else F.Run=On;
  if(S==CR_SUCCESS || On || !F.RetainOffToken)*Owned=On;
  return S;
}
STATIC CR_LUCID_PLL Setup(VOID)
{
  CR_LUCID_PLL P={0};memset(&F,0,sizeof(F));F.Quiet=TRUE;F.Retired=TRUE;
  F.Reg[0x0c/4]=0x20000;
  CR_LUCID_PLL_IO I={&F,0xaf01000,0x44,Ready,Read,Write,Fence,Delay,Vote};
  assert(CrLucidPllInit(&P,&I,CR_LUCID_PLL_SM8250_DISP1)==CR_SUCCESS&&!F.Calls);
  return P;
}
STATIC VOID IsOff(CR_LUCID_PLL *P)
{
  assert(!CrLucidPllNeedsAccess(P)&&!F.Cal&&!F.Run);
  assert(!(F.Reg[0]&1)&&!(F.Reg[0x0c/4]&7)&&!(F.Reg[0x38/4]&1));
  assert(F.Reg[0x0c/4]==0x20000);
}
int main(VOID)
{
  CR_LUCID_PLL Source=Setup(),Alias={0};Alias.Io=Source.Io;
  assert(CrLucidPllInit(&Alias,&Alias.Io,CR_LUCID_PLL_SM8250_DISP1)==CR_SUCCESS);
  assert(CrLucidPllAcquire(&Alias)==CR_SUCCESS);
  assert(CrLucidPllRelease(&Alias)==CR_SUCCESS);IsOff(&Alias);
  CR_LUCID_PLL P=Setup();assert(CrLucidPllAcquire(&P)==CR_SUCCESS);
  UINT32 StartCalls=F.Calls;assert(P.Ready&&F.Runs==2&&F.Run&&!F.Cal);
  assert(CrLucidPllCheck(&P)==CR_SUCCESS);
  UINT32 W=F.Writes;assert(CrLucidPllAcquire(&P)==CR_SUCCESS&&F.Writes==W);
  assert(CrLucidPllUninit(&P)==CR_BUSY);
  F.Calls=0;assert(CrLucidPllRelease(&P)==CR_SUCCESS);UINT32 StopCalls=F.Calls;IsOff(&P);
  OracleCount=0;AndroidLucidReference();assert(OracleCount==F.TraceCount);
  for(UINT32 I=0;I<OracleCount;++I)
    assert(Oracle[I].Offset==F.Trace[I].Offset&&Oracle[I].Value==F.Trace[I].Value);
  assert(CrLucidPllUninit(&P)==CR_SUCCESS);
  for(UINT32 Warning=0;Warning<2;++Warning)
  for(UINT32 Commit=0;Commit<2;++Commit)for(UINT32 N=1;N<=StartCalls;++N){
    P=Setup();F.Fail=N;F.Commit=(BOOLEAN)Commit;F.Warning=(BOOLEAN)Warning;
    assert(CrLucidPllAcquire(&P)==(Warning?CR_NOT_READY:CR_DEVICE_ERROR)&&F.Calls==N&&!P.Ready);
    if(F.Writes)assert(P.Touched&&CrLucidPllNeedsAccess(&P));
    F.Fail=0;
    if(CrLucidPllNeedsAccess(&P)){W=F.Writes;
      assert(CrLucidPllAcquire(&P)==CR_NOT_READY&&F.Writes==W);
      assert(CrLucidPllUninit(&P)==CR_BUSY);}
    assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  }
  for(UINT32 Warning=0;Warning<2;++Warning)
  for(UINT32 Commit=0;Commit<2;++Commit)for(UINT32 N=1;N<=StopCalls;++N){
    P=Setup();assert(CrLucidPllAcquire(&P)==CR_SUCCESS);
    F.Calls=0;F.Fail=N;F.Commit=(BOOLEAN)Commit;F.Warning=(BOOLEAN)Warning;
    assert(CrLucidPllRelease(&P)==(Warning?CR_NOT_READY:CR_DEVICE_ERROR)&&F.Calls==N&&CrLucidPllNeedsAccess(&P));
    F.Fail=0;assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  }
  P=Setup();F.Posted=TRUE;assert(CrLucidPllAcquire(&P)==CR_SUCCESS);
  assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();F.LockNever=TRUE;
  assert(CrLucidPllAcquire(&P)==CR_TIMEOUT&&P.Touched&&P.CalibrationOwned&&!P.Ready&&F.DelayUs==1500);
  assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();F.LockStale=TRUE;
  assert(CrLucidPllAcquire(&P)==CR_TIMEOUT&&P.Touched&&!F.Runs&&F.DelayUs==1500);
  assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();F.Reg[0]|=1U<<27;
  assert(CrLucidPllAcquire(&P)==CR_SUCCESS&&F.Runs==2);
  assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  /* Provider may retain its failed cleanup token after voltage changed. */
  P=Setup();F.Commit=TRUE;F.RetainOffToken=TRUE;F.FailCalOff=TRUE;
  assert(CrLucidPllAcquire(&P)==CR_DEVICE_ERROR&&P.CalibrationOwned&&P.OperatingOwned&&!F.Cal&&F.Run);
  F.FailCalOff=FALSE;assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();F.FailRunOnToken=TRUE;
  assert(CrLucidPllAcquire(&P)==CR_DEVICE_ERROR&&P.CalibrationOwned&&P.OperatingOwned&&F.Cal&&!F.Run);
  assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();assert(CrLucidPllAcquire(&P)==CR_SUCCESS);F.Quiet=FALSE;W=F.Writes;
  assert(CrLucidPllRelease(&P)==CR_NOT_READY&&P.Touched&&F.Writes==W&&F.Run);
  F.Quiet=TRUE;assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();F.Retired=FALSE;
  assert(CrLucidPllAcquire(&P)==CR_NOT_READY&&!CrLucidPllNeedsAccess(&P)&&!F.Writes);
  P=Setup();F.Reg[0]=1U<<20;
  assert(CrLucidPllAcquire(&P)==CR_UNSUPPORTED&&!P.Touched&&!F.Writes&&P.CalibrationOwned);
  assert(CrLucidPllRelease(&P)==CR_SUCCESS&&!F.Cal);
  P=Setup();F.Reg[0x38/4]=1;
  assert(CrLucidPllAcquire(&P)==CR_BUSY&&!P.Touched&&!F.Writes);
  /* A foreign running source wasn't mutated; its calibration vote can drop
   * without proving its unrelated source is off. Model that separate rail
   * owner as still supplying it; no cleanup PLL write is allowed. */
  F.ForeignOwner=TRUE;
  assert(CrLucidPllRelease(&P)==CR_SUCCESS&&!F.Writes&&!F.Cal&&!F.Run);
  assert(F.Reg[0x38/4]==1&&!CrLucidPllNeedsAccess(&P));
  P=Setup();assert(CrLucidPllAcquire(&P)==CR_SUCCESS);F.Reg[0]|=1U<<20;W=F.Writes;
  assert(CrLucidPllRelease(&P)==CR_NOT_READY&&P.Touched&&F.Writes==W);
  F.Reg[0]&=~(1U<<20);assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  CONST UINT32 Drop[]={0,0x0c,0x38};
  for(UINT32 I=0;I<3;++I){
    P=Setup();assert(CrLucidPllAcquire(&P)==CR_SUCCESS);F.Drop=Drop[I]+1;
    assert(CrLucidPllRelease(&P)==CR_DEVICE_ERROR&&P.Touched&&F.Run);
    F.Drop=0;assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  }
  P=Setup();assert(CrLucidPllAcquire(&P)==CR_SUCCESS);F.Reactivate=TRUE;
  assert(CrLucidPllRelease(&P)==CR_DEVICE_ERROR&&P.Touched&&F.Run);
  F.Reactivate=FALSE;assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
  P=Setup();++P.Io.Physical;assert(CrLucidPllAcquire(&P)==CR_INVALID_PARAMETER&&!F.Writes);
#ifdef _KERNEL_MODE
  P=Setup();TestIrql=2;
  assert(CrLucidPllAcquire(&P)==CR_INVALID_PARAMETER&&!F.Calls);
  TestIrql=0;assert(CrLucidPllAcquire(&P)==CR_SUCCESS);UINT32 Calls=F.Calls;
  TestIrql=2;
  assert(CrLucidPllCheck(&P)==CR_INVALID_PARAMETER&&F.Calls==Calls);
  assert(CrLucidPllRelease(&P)==CR_INVALID_PARAMETER&&F.Calls==Calls&&P.Ready);
  TestIrql=0;assert(CrLucidPllRelease(&P)==CR_SUCCESS);IsOff(&P);
#endif
  printf("PASS: actual Android Lucid operation oracle; %u acquire + %u release callbacks x2 side-effect modes x2 error/warning statuses; power/lock/ownership/posted/drop/retained retry\n",StartCalls,StopCalls);
  return 0;
}
