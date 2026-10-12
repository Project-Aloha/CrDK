/* Real helper qualification/continuation with hostile secure replies. MIT */
#include <Library/PilSmcccLib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *D,UINTN N,UINT8 V) {return memset(D,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) {return memcmp(A,B,N);}
#else
static KIRQL Irql;
KIRQL KeGetCurrentIrql(VOID) {return Irql;}
#endif
static UINT32 Calls,Shutdowns,ActualEl=1,ProbeCalls,FailProbe,InterruptProbe,TransportFailure;
static BOOLEAN Interrupt,Forever,ProbeMode,Legacy,NoConvention;
static CR_PIL_SMC_ARGS Expected,Reply;
UINT32 CrPilArm64CurrentEl(VOID) {return ActualEl;}
VOID CrPilArm64Smccc(CR_PIL_SMC_ARGS *A) {
  ++Calls;
  if (ProbeMode) {
    if (A->X[0]==1) {
      assert(A->X[6]==0xfedcba9876543210ULL);
      for(UINT32 I=1;I<8;++I) if(I!=6) assert(A->X[I]==Expected.X[I]);
      if(Forever) {A->X[0]=1;A->X[6]=0xfedcba9876543210ULL;return;}
    } else {
      Expected=*A;++ProbeCalls;
      /* Qualification must never submit a state-changing PAS call. */
      assert((A->X[0]&0xffffU)==0x601U || (A->X[0]&0xffffU)==0x207U);
      assert(A->X[1]==1 && A->X[3]==0 && A->X[4]==0 && A->X[5]==0 && A->X[6]==0 && A->X[7]==0);
      if(ProbeCalls==InterruptProbe) {A->X[0]=1;A->X[1]=99;A->X[6]=0xfedcba9876543210ULL;return;}
    }
    BOOLEAN Supported=!(NoConvention && (Expected.X[0]&0x80000000U)!=0) &&
      !(Legacy && Expected.X[0]==0xc2000601U) && ProbeCalls!=FailProbe;
    A->X[0]=ProbeCalls==TransportFailure ? CR_MAX_UINT64 : 0;A->X[1]=Supported ? 1 : 0;return;
  }
  if (A->X[0]==1) {
    assert(A->X[6]==0x1234);
    for(UINT32 I=1;I<8;++I) if(I!=6) assert(A->X[I]==Expected.X[I]);
    if(Forever) {A->X[0]=1;A->X[6]=0x1234;return;}
    *A=Reply;return;
  }
  if ((A->X[0]&255)==6) ++Shutdowns;
  if (Interrupt) {Expected=*A;A->X[0]=1;A->X[1]=99;A->X[3]=0;A->X[6]=0x1234;Interrupt=FALSE;return;}
  *A=Reply;
}
static CR_PIL_TARGET Target={.PasId=1,.MemoryBase=0x85e00000,.MemorySize=0x02100000};
static CR_PIL_SMC_ARGS Init(void) {return (CR_PIL_SMC_ARGS){{0x42000201,0x82,1,0x12345000,0,0,0,0}};}
static VOID ProbeTests(VOID) {
  ProbeMode=TRUE;Forever=FALSE;
  for(UINT32 El=0;El<=3;++El) if(El!=1) {
    CR_PIL_SMCCC T={0};ActualEl=El;UINT32 Before=Calls;
    assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS && CrPilSmcccProbe(&T)==CR_UNSUPPORTED);
    assert(Calls==Before && !T.Qualified && !T.Interrupted && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  }
  ActualEl=1;
  for(UINT32 Mode=0;Mode<2;++Mode) {
    CR_PIL_SMCCC T={0};Legacy=Mode!=0;ProbeCalls=FailProbe=InterruptProbe=0;
    assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS && CrPilSmcccProbe(&T)==CR_SUCCESS && T.Qualified);
    assert(ProbeCalls==(Mode ? 8 : 7) && T.SmcBase==(Mode ? 0x02000000U : 0x42000000U));
    UINT32 Before=Calls;assert(CrPilSmcccProbe(&T)==CR_SUCCESS && Calls==Before && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  }
  Legacy=FALSE;NoConvention=TRUE;ProbeCalls=0;
  CR_PIL_SMCCC T={0};assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS);
  assert(CrPilSmcccProbe(&T)==CR_UNSUPPORTED && ProbeCalls==2 && !T.Qualified && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  NoConvention=FALSE;
  for(UINT32 Failure=2;Failure<=6;++Failure) {
    memset(&T,0,sizeof(T));FailProbe=Failure;ProbeCalls=InterruptProbe=0;
    assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS);
    assert(CrPilSmcccProbe(&T)==CR_UNSUPPORTED && !T.Qualified && !T.Interrupted);
    assert(ProbeCalls==Failure && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  }
  memset(&T,0,sizeof(T));FailProbe=7;ProbeCalls=InterruptProbe=0;
  assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS && CrPilSmcccProbe(&T)==CR_SUCCESS);
  assert(T.Qualified && T.PasSupportKnown && T.PasSupportResult==0 && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  for(UINT32 Failure=2;Failure<=7;++Failure) {
    memset(&T,0,sizeof(T));TransportFailure=Failure;FailProbe=ProbeCalls=InterruptProbe=0;
    assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS && CrPilSmcccProbe(&T)==CR_UNSUPPORTED);
    assert(!T.Qualified && !T.PasSupportKnown && !T.Interrupted && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  }
  TransportFailure=0;
  FailProbe=0;
  for(UINT32 Pending=1;Pending<=7;++Pending) {
    memset(&T,0,sizeof(T));ProbeCalls=0;InterruptProbe=Pending;Forever=TRUE;
    assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS);
    assert(CrPilSmcccProbe(&T)==CR_TIMEOUT && T.Interrupted && !T.Qualified && ProbeCalls==Pending);
    CR_PIL_SMC_ARGS Saved=T.Original;UINT64 Cookie=T.ResumeCookie;UINT32 Step=T.ProbeStep;
    assert(CrPilSmcccProbe(&T)==CR_TIMEOUT && T.Interrupted && T.ProbeStep==Step &&
      T.ResumeCookie==Cookie && !memcmp(&T.Original,&Saved,sizeof(Saved)) && ProbeCalls==Pending);
    assert(CrPilSmcccUninitialize(&T)==CR_NOT_READY && CrPilSmcccInitialize(&T,&Target)==CR_ALREADY_STARTED);
    Forever=FALSE;assert(CrPilSmcccProbe(&T)==CR_SUCCESS && T.Qualified && !T.Interrupted);
    assert(ProbeCalls==7 && CrPilSmcccUninitialize(&T)==CR_SUCCESS);
  }
  ProbeMode=FALSE;
}
int main(void) {
  CR_PIL_SMCCC T={0};CR_PIL_SMC_ARGS A;
  assert(CrPilSmcccInitialize(&T,&Target)==CR_SUCCESS);
  assert(CrPilSmcccInitialize(&T,&Target)==CR_ALREADY_STARTED && Calls==0);
  Reply=(CR_PIL_SMC_ARGS){{0xfffffffffffffffcULL,0x8877665544332211ULL,2,3,4,5,6,7}};
  A=Init();assert(CrPilSmcccCall(&T,&A)==CR_SUCCESS && !memcmp(&A,&Reply,sizeof(A)) && Calls==1 && !T.Interrupted);
  UINT32 Before=Calls;A=Init();A.X[0]=0xc2000206;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=Init();A.X[2]=2;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=Init();A.X[3]=0x100000000ULL;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=Init();A.X[3]=0x12345001;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=Init();A.X[5]=1;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=(CR_PIL_SMC_ARGS){{0x42000202,3,1,0x85e00000,0x02100001,0,0,0}};
  assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER && Calls==Before);
  A=(CR_PIL_SMC_ARGS){{1,0x82,1,0x12345000,0,0,0x1234,0}};
  assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=Init();Interrupt=TRUE;assert(CrPilSmcccCall(&T,&A)==CR_SUCCESS && T.Interrupted && T.ResumeCookie==0x1234);
  CR_PIL_SMC_ARGS Saved=T.Original;
  assert(CrPilSmcccInitialize(&T,&Target)==CR_ALREADY_STARTED && T.Interrupted &&
         T.ResumeCookie==0x1234 && !memcmp(&Saved,&T.Original,sizeof(Saved)));
  assert(CrPilSmcccUninitialize(&T)==CR_NOT_READY);
  A=Init();assert(CrPilSmcccCall(&T,&A)==CR_NOT_READY);
  A=Expected;A.X[0]=1;A.X[6]=0x1235;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A.X[6]=0x1234;A.X[1]=99;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER);
  A=Expected;A.X[0]=1;A.X[6]=0x1234;Forever=TRUE;
  assert(CrPilSmcccCall(&T,&A)==CR_SUCCESS && T.Interrupted);
  A=(CR_PIL_SMC_ARGS){{0x42000206,1,1,0,0,0,0,0}};Before=Calls;
  assert(CrPilSmcccCall(&T,&A)==CR_TIMEOUT && Calls==Before+256 && Shutdowns==0 && T.Interrupted);
  Forever=FALSE;Reply=(CR_PIL_SMC_ARGS){{0,0,2,3,4,5,6,7}};
  assert(CrPilSmcccCall(&T,&A)==CR_SUCCESS && Shutdowns==1 && !T.Interrupted && !memcmp(&A,&Reply,sizeof(A)));
#ifdef _KERNEL_MODE
  Irql=DISPATCH_LEVEL;A=Init();Before=Calls;assert(CrPilSmcccCall(&T,&A)==CR_INVALID_PARAMETER && Calls==Before);Irql=PASSIVE_LEVEL;
#endif
  assert(CrPilSmcccUninitialize(&T)==CR_SUCCESS && !T.Initialized);
  ProbeTests();
  puts("PIL SMCCC: exact qualified frames/raw returns, opaque resume cookies, hostile continuation, bounded drain before shutdown and retained recovery PASS");return 0;
}
