/* Actual power journal: loss at every callback, retained cleanup and cold restart.
   SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#undef NULL
#include <Library/Dwc3PowerCrLib.h>
static CR_DWC3_POWER P;
static BOOLEAN Paths[2],Domain,Clocks[6],Reset,Denied,After,FalseSuccess,Replay;
static UINT32 Calls,Fail,Time;
static CR_STATUS Failure;
#ifdef _KERNEL_MODE
static KIRQL Irql;
KIRQL KeGetCurrentIrql(VOID){return Irql;}
VOID KeStallExecutionProcessor(ULONG Us){Time+=Us;}
NTSTATUS KeDelayExecutionThread(KPROCESSOR_MODE M,BOOLEAN A,LARGE_INTEGER *I){(void)M;(void)A;Time+=(UINT32)(-I->QuadPart/10);return STATUS_SUCCESS;}
#else
UINTN EFIAPI MicroSecondDelay(UINTN Us){Time+=(UINT32)Us;return Us;}
#endif
static CR_STATUS Step(VOID){return ++Calls==Fail?Failure:CR_SUCCESS;}
static CR_STATUS Access(VOID *C){assert(C==&P);return Denied?CR_NOT_READY:Step();}
static CR_STATUS Vote(BOOLEAN *Hardware,BOOLEAN On,BOOLEAN *Owned){
  CR_STATUS S=Step();
  if(S!=CR_SUCCESS&&!After)return S;
  if(On&&!Replay)assert(!*Hardware);*Hardware=On;*Owned=On;
  if(FalseSuccess&&On)*Owned=FALSE;
  return S;
}
static CR_STATUS Path(VOID *C,UINT32 I,BOOLEAN On,UINT64 A,UINT64 Peak,BOOLEAN *Owned){
  assert(C==&P&&I<2);
  if(On){assert(!Domain);if(!I)assert(A==(P.SuperSpeed?1000000:240000)&&Peak==(P.SuperSpeed?2500000:700000));else assert(!A&&Peak==40000);}
  else {assert(!Domain&&!A&&!Peak);if(!I)assert(!Paths[1]);}
  return Vote(&Paths[I],On,Owned);
}
static CR_STATUS DomainVote(VOID *C,BOOLEAN On,BOOLEAN *Owned){
  assert(C==&P&&Paths[0]&&Paths[1]);for(UINT32 I=0;I<6;++I)assert(!Clocks[I]);
  if(On)assert(!Reset);return Vote(&Domain,On,Owned);
}
static CR_STATUS Clock(VOID *C,CONST CHAR8 *N,UINT64 Rate,BOOLEAN On,BOOLEAN *Owned){
  assert(C==&P&&Domain&&Paths[0]&&Paths[1]&&!Reset);UINT32 I;
  for(I=0;I<6;++I)if(!strcmp(N,CrDwc3PowerClocks[I].Name))break;
  assert(I<6&&Rate==(On?CrDwc3PowerClocks[I].RateHz:0));
  if(On){for(UINT32 J=0;J<I;++J)assert(Clocks[J]);}
  else {for(UINT32 J=I+1;J<6;++J)assert(!Clocks[J]);}
  return Vote(&Clocks[I],On,Owned);
}
static CR_STATUS ResetVote(VOID *C,BOOLEAN On){
  assert(C==&P&&Paths[0]&&Paths[1]);CR_STATUS S=Step();if(S==CR_SUCCESS||After)Reset=On;return S;
}
static CR_STATUS Divider(VOID *C){assert(C==&P&&Domain&&!Reset&&!Clocks[5]);return Step();}
static CONST CR_DWC3_POWER_IO Io={&P,Access,Path,DomainVote,Clock,ResetVote,Divider};
static VOID Fixture(VOID){
  memset(&P,0,sizeof(P));memset(Paths,0,sizeof(Paths));memset(Clocks,0,sizeof(Clocks));
  Domain=Reset=Denied=After=FalseSuccess=Replay=FALSE;Calls=Fail=Time=0;Failure=CR_TIMEOUT;
  assert(CrDwc3PowerInit(&P,&Io)==CR_SUCCESS);
}
static VOID Drain(VOID){
  Fail=0;Denied=After=FalseSuccess=FALSE;
  /* Hardware may already be off after a failed release. The provider's
     absolute retry retires the retained reference without reacquiring it. */
  assert(CrDwc3PowerQuiesce(&P)==CR_SUCCESS&&!Reset);
  assert(CrDwc3PowerRelease(&P)==CR_SUCCESS);
  assert(!Domain&&!Paths[0]&&!Paths[1]);for(UINT32 I=0;I<6;++I)assert(!Clocks[I]);
  assert(!P.Started&&!P.Ready&&!P.ResetTouched);
  assert(CrDwc3PowerUninit(&P)==CR_SUCCESS);
}
int main(void){
  Fixture();assert(CrDwc3PowerAcquire(&P,TRUE)==CR_SUCCESS&&P.Ready);UINT32 AcquireCalls=Calls;
  assert(Time==1000);UINT32 Before=Calls;
  assert(CrDwc3PowerAcquire(&P,TRUE)==CR_ALREADY_STARTED&&Calls==Before);
  assert(CrDwc3PowerRelease(&P)==CR_BUSY&&CrDwc3PowerUninit(&P)==CR_BUSY&&Calls==Before);
  assert(CrDwc3PowerQuiesce(&P)==CR_SUCCESS);assert(CrDwc3PowerRelease(&P)==CR_SUCCESS);UINT32 ReleaseCalls=Calls-AcquireCalls;
  assert(Time==1010&&!Reset);assert(CrDwc3PowerAcquire(&P,FALSE)==CR_SUCCESS&&P.Ready);Drain();
  UINT32 Cases=0;
  for(UINT32 Ss=0;Ss<2;++Ss)for(UINT32 Late=0;Late<2;++Late)for(UINT32 Code=0;Code<2;++Code){
    for(UINT32 F=1;F<=AcquireCalls;++F){
      Fixture();After=(BOOLEAN)Late;Failure=Code?1:CR_TIMEOUT;Fail=F;
      assert(CrDwc3PowerAcquire(&P,(BOOLEAN)Ss)!=CR_SUCCESS&&!P.Ready);
      Drain();++Cases;
    }
    for(UINT32 F=1;F<=ReleaseCalls;++F){
      Fixture();assert(CrDwc3PowerAcquire(&P,(BOOLEAN)Ss)==CR_SUCCESS);
      Calls=0;Fail=F;After=(BOOLEAN)Late;Failure=Code?1:CR_TIMEOUT;
      CR_STATUS S=CrDwc3PowerQuiesce(&P);if(S==CR_SUCCESS)S=CrDwc3PowerRelease(&P);
      assert(S!=CR_SUCCESS&&P.Started&&!P.Ready);
      /* No lower prerequisite may retire while a clock may still be held. */
      for(UINT32 I=0;I<6;++I)if(P.Clocks[I])assert(P.Domain&&P.Paths[0]&&P.Paths[1]);
      if(P.Domain)assert(P.Paths[0]&&P.Paths[1]);
      Drain();++Cases;
    }
  }
  UINT32 Resumes=0;
  for(UINT32 Ss=0;Ss<2;++Ss)for(UINT32 Late=0;Late<2;++Late)for(UINT32 Code=0;Code<2;++Code)
    for(UINT32 F=1;F<=AcquireCalls;++F){
      Fixture();After=(BOOLEAN)Late;Failure=Code?1:CR_TIMEOUT;Fail=F;
      assert(CrDwc3PowerAcquire(&P,(BOOLEAN)Ss)!=CR_SUCCESS&&!P.Ready);
      Fail=0;After=FALSE;
      if(P.Started){
        CR_DWC3_POWER Saved=P;Before=Calls;
        assert(CrDwc3PowerResume(&P,(BOOLEAN)!Ss)==CR_INVALID_PARAMETER&&Calls==Before&&!memcmp(&P,&Saved,sizeof(P)));
        Denied=TRUE;assert(CrDwc3PowerResume(&P,(BOOLEAN)Ss)==CR_NOT_READY&&Calls==Before);Denied=FALSE;
        Replay=TRUE;assert(CrDwc3PowerResume(&P,(BOOLEAN)Ss)==CR_SUCCESS&&P.Ready&&!P.AcquirePending);
        Before=Calls;assert(CrDwc3PowerResume(&P,(BOOLEAN)Ss)==CR_NOT_READY&&Calls==Before);Replay=FALSE;
      }else assert(CrDwc3PowerAcquire(&P,(BOOLEAN)Ss)==CR_SUCCESS);
      assert(CrDwc3PowerQuiesce(&P)==CR_SUCCESS);Before=Calls;
      assert(CrDwc3PowerResume(&P,(BOOLEAN)Ss)==CR_NOT_READY&&Calls==Before);
      Drain();++Resumes;
    }
  Fixture();assert(CrDwc3PowerAcquire(&P,FALSE)==CR_SUCCESS);Before=Calls;Denied=TRUE;
  assert(CrDwc3PowerQuiesce(&P)==CR_NOT_READY&&P.ResetTouched&&!P.Ready&&Calls==Before);
  assert(CrDwc3PowerRelease(&P)==CR_BUSY&&Calls==Before);Drain();
  Fixture();FalseSuccess=TRUE;assert(CrDwc3PowerAcquire(&P,FALSE)==CR_DEVICE_ERROR&&P.Paths[0]);Drain();
  Fixture();assert(CrDwc3PowerAcquire(&P,2)==CR_INVALID_PARAMETER&&!Calls);Drain();
#ifdef _KERNEL_MODE
  Fixture();Irql=DISPATCH_LEVEL;assert(CrDwc3PowerAcquire(&P,FALSE)==CR_INVALID_PARAMETER&&!Calls);
  assert(CrDwc3PowerQuiesce(&P)==CR_INVALID_PARAMETER&&CrDwc3PowerRelease(&P)==CR_INVALID_PARAMETER);
  Irql=PASSIVE_LEVEL;Drain();
#endif
  printf("DWC3 power: %u failure phases + %u exact retained resumes, retained votes, cold restart, lost access and strict completion PASS\n",Cases,Resumes);
  return 0;
}
