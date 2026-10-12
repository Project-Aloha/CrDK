/* Actual command PHY, upstream trace oracle, posted writes and fault leases.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef _KERNEL_MODE
#undef NULL
#endif
#include "../Library/MdssDsiCmdCrLib/Internal.h"
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V) { return memset(P,V,N); }
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N) { return memcmp(A,B,N); }
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N) { return memmove(D,S,N); }
#endif

typedef struct { UINT32 Offset,Value; } WRITE;
typedef struct {
  UINT32 Reg[0x7c0/4],Calls,Fail,Writes,Drop,ReadDelay,TotalDelay;
  UINT32 PendingOffset,PendingValue,PendingReads,TraceCount;
  BOOLEAN CommitOnError,Posted,Reactivate,Stop,LockNever,RefNever,StaleLock;
  WRITE Trace[1024];
} FIXTURE;
STATIC FIXTURE F;
STATIC WRITE Oracle[256];
STATIC UINT32 OracleCount;
extern void AndroidPhyReference(UINT32 Revision);
extern void MainlinePllReference(void);
/* These reference functions are compiled from the pinned upstream bodies.
 * They never include the production register constants or implementation. */
void OracleWrite(UINT32 Offset,UINT32 Value)
{
  assert(OracleCount<sizeof(Oracle)/sizeof(Oracle[0]));
  Oracle[OracleCount++]=(WRITE){Offset,Value};
}
STATIC CR_STATUS Tick(VOID) { return ++F.Calls==F.Fail?CR_DEVICE_ERROR:CR_SUCCESS; }
STATIC CR_STATUS Ready(VOID *C,CR_MDSS_CMD_WINDOW W)
{ assert(C==&F&&W==CrMdssCmdPhy); return Tick(); }
STATIC CR_STATUS Read(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 *V)
{
  assert(C==&F&&W==CrMdssCmdPhy&&!(O&3)&&O<sizeof(F.Reg));
  CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  if(F.PendingReads&&!--F.PendingReads)F.Reg[F.PendingOffset/4]=F.PendingValue;
  if(O==0x140)*V=F.RefNever?0:1;
  else if(O==0x6b0)*V=F.LockNever?0:((F.Reg[0x3c/4]&1)||F.StaleLock);
  else *V=F.Reg[O/4];
  return CR_SUCCESS;
}
STATIC CR_STATUS Write(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V)
{
  assert(C==&F&&W==CrMdssCmdPhy&&!(O&3)&&O<sizeof(F.Reg));
  CR_STATUS S=Tick();++F.Writes;
  if(S!=CR_SUCCESS&&!F.CommitOnError)return S;
  assert(O!=0x140&&O!=0x6b0); /* status is never a programming register */
  assert(F.TraceCount<sizeof(F.Trace)/sizeof(F.Trace[0]));
  F.Trace[F.TraceCount++]=(WRITE){O,V};
  if(F.Stop)assert((V&~F.Reg[O/4])==0); /* cleanup never turns a bit on */
  if(F.Drop==O+1)return S;
  if(F.Posted){F.PendingOffset=O;F.PendingValue=V;F.PendingReads=F.ReadDelay;}
  else F.Reg[O/4]=V;
  if(O==0x024&&!(V&0xff)&&F.Reactivate)F.Reg[0x014/4]|=0x20;
  return S;
}
STATIC CR_STATUS Fence(VOID *C)
{
  assert(C==&F);CR_STATUS S=Tick();if(S!=CR_SUCCESS)return S;
  if(F.Posted&&!F.ReadDelay)F.Reg[F.PendingOffset/4]=F.PendingValue;
  return CR_SUCCESS;
}
STATIC CR_STATUS Delay(VOID *C,UINT32 Us)
{ assert(C==&F);F.TotalDelay+=Us;return Tick(); }
STATIC CR_MDSS_CMD Setup(VOID)
{
  CR_MDSS_CMD D;CR_MDSS_CMD_CONFIG C;
  memset(&F,0,sizeof(F));memset(&D,0,sizeof(D));
  CrMdssCmdHdk8245Config(&C);CrMdssCmdR66451Profile(&D.Profile);D.Profile.Config=C;assert(CrMdssCmdBuildPlanProfile(&D.Profile,&D.Plan)==CR_SUCCESS);
  D.Initialized=TRUE;D.Io.Context=&F;D.Io.Physical[CrMdssCmdPhy]=0xae94400;
  D.Io.Bytes[CrMdssCmdPhy]=0x7c0;D.Io.Ready=Ready;D.Io.Read32=Read;
  D.Io.Write32=Write;D.Io.Fence=Fence;D.Io.DelayUs=Delay;
  return D;
}
STATIC VOID Off(VOID)
{
  CONST WRITE Gates[]={{0xa4,0x60},{0x14,0x20},{0x3c,1},{0x1c,0xff},
                      {0x528,0xff},{0x214,0xff},{0xa0,0xff},{0x24,0xff}};
  for(UINT32 I=0;I<sizeof(Gates)/sizeof(Gates[0]);++I)
    assert(!(F.Reg[Gates[I].Offset/4]&Gates[I].Value));
}
STATIC VOID TraceReference(UINT32 Revision)
{
  CR_MDSS_CMD D=Setup();F.Reg[0]=Revision;
  assert(CmdPhyStart(&D)==CR_SUCCESS&&D.PhyTouched);
  OracleCount=0;AndroidPhyReference(Revision);
  assert(OracleCount>60&&OracleCount<F.TraceCount);
  for(UINT32 I=0;I<OracleCount;++I)
    assert(Oracle[I].Offset==F.Trace[I].Offset&&Oracle[I].Value==F.Trace[I].Value);
  UINT32 Start=OracleCount;
  assert(F.Trace[Start].Offset==0x528&&F.Trace[Start].Value==0xc0);
  ++Start;OracleCount=0;MainlinePllReference();
  for(UINT32 I=0;I<OracleCount;++I)
    assert(Oracle[I].Offset==F.Trace[Start+I].Offset&&Oracle[I].Value==F.Trace[Start+I].Value);
  /* Android clock tree: VCO/2/1 => lane; lane/2/3 => pixel. Actual 18-bit
   * integer fraction is 0x442526; no claim that requested Hz is exact. */
  UINT64 Q=((UINT64)F.Reg[0x5e0/4]<<18)|F.Reg[0x5e4/4]|
           ((UINT64)F.Reg[0x5e8/4]<<8)|((UINT64)F.Reg[0x5ec/4]<<16);
  assert(Q==4465958&&Q*38400000ULL==171492787200000ULL);
  assert(F.Reg[0x654/4]==1&&F.Reg[0x10/4]==0x31&&F.Reg[0x14/4]==0x21);
  assert(F.Reg[0x650/4]==0&&F.Reg[0x760/4]==0x22&&F.Reg[0x748/4]==0xa0);
  UINT32 LockStart=Start+OracleCount;
  CONST WRITE Tail[]={{0x650,0},{0x654,1},{0x10,0x31},{0x14,1},{0x3c,1},
                      {0x128,1},{0x128,0},{0x30,4},{0x14,0x31},{0x1c,1},{0x14,0x21}};
  assert(F.TraceCount==LockStart+sizeof(Tail)/sizeof(Tail[0]));
  for(UINT32 I=0;I<sizeof(Tail)/sizeof(Tail[0]);++I)
    assert(F.Trace[LockStart+I].Offset==Tail[I].Offset&&F.Trace[LockStart+I].Value==Tail[I].Value);
  F.Stop=TRUE;assert(CmdPhyStop(&D)==CR_SUCCESS&&!D.PhyTouched);Off();
}
int main(VOID)
{
  TraceReference(0);TraceReference(0x20);
  CR_MDSS_CMD D=Setup();assert(CmdPhyStart(&D)==CR_SUCCESS);
  UINT32 StartCalls=F.Calls;assert(StartCalls>600);
  UINT32 Before=F.Writes;assert(CmdPhyStart(&D)==CR_NOT_READY&&F.Writes==Before);
  F.Stop=TRUE;F.Calls=0;assert(CmdPhyStop(&D)==CR_SUCCESS);UINT32 StopCalls=F.Calls;
  for(UINT32 Commit=0;Commit<2;++Commit)for(UINT32 N=1;N<=StartCalls;++N){
    D=Setup();F.Fail=N;F.CommitOnError=(BOOLEAN)Commit;
    assert(CmdPhyStart(&D)==CR_DEVICE_ERROR&&F.Calls==N);
    if(F.Writes)assert(D.PhyTouched);
    if(D.PhyTouched){UINT32 W=F.Writes;F.Fail=0;
      assert(CmdPhyStart(&D)==CR_NOT_READY&&F.Writes==W);}
    F.Fail=0;F.Stop=TRUE;
    assert(CmdPhyStop(&D)==CR_SUCCESS&&!D.PhyTouched);Off();
  }
  for(UINT32 Commit=0;Commit<2;++Commit)for(UINT32 N=1;N<=StopCalls;++N){
    D=Setup();assert(CmdPhyStart(&D)==CR_SUCCESS);F.Stop=TRUE;F.Calls=0;
    F.Fail=N;F.CommitOnError=(BOOLEAN)Commit;
    assert(CmdPhyStop(&D)==CR_DEVICE_ERROR&&D.PhyTouched&&F.Calls==N);
    F.Fail=0;assert(CmdPhyStop(&D)==CR_SUCCESS&&!D.PhyTouched);Off();
  }
  /* Successful posted writes are observable only after the fence. */
  D=Setup();F.Posted=TRUE;assert(CmdPhyStart(&D)==CR_SUCCESS);
  F.Stop=TRUE;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  D=Setup();assert(CmdPhyStart(&D)==CR_SUCCESS);F.Stop=TRUE;F.Posted=TRUE;F.ReadDelay=3;
  assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  /* Each active gate, including asynchronous reactivation, is a real fence. */
  CONST UINT32 Gates[]={0xa4,0x14,0x3c,0x1c,0x528,0x214,0xa0,0x24};
  for(UINT32 I=0;I<sizeof(Gates)/sizeof(Gates[0]);++I){
    D=Setup();assert(CmdPhyStart(&D)==CR_SUCCESS);F.Reg[0xa4/4]=0x60;
    F.Drop=Gates[I]+1;F.Stop=TRUE;
    assert(CmdPhyStop(&D)==CR_TIMEOUT&&D.PhyTouched);
    F.Drop=0;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  }
  D=Setup();assert(CmdPhyStart(&D)==CR_SUCCESS);F.Reactivate=TRUE;F.Stop=TRUE;
  assert(CmdPhyStop(&D)==CR_DEVICE_ERROR&&D.PhyTouched);
  F.Reactivate=FALSE;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  D=Setup();F.RefNever=TRUE;
  assert(CmdPhyStart(&D)==CR_TIMEOUT&&!D.PhyTouched&&!F.Writes&&F.TotalDelay==1000);
  D=Setup();F.LockNever=TRUE;
  assert(CmdPhyStart(&D)==CR_TIMEOUT&&D.PhyTouched&&F.TotalDelay==5001);
  assert(!(F.Reg[0x14/4]&0x20)&&!F.Reg[0x1c/4]);
  F.Stop=TRUE;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  D=Setup();F.StaleLock=TRUE;
  assert(CmdPhyStart(&D)==CR_TIMEOUT&&D.PhyTouched&&F.TotalDelay==1000);
  assert(F.Writes==2&&!F.Reg[0x3c/4]);
  F.Stop=TRUE;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  D=Setup();F.Drop=0x5e0+1;
  assert(CmdPhyStart(&D)==CR_DEVICE_ERROR&&D.PhyTouched);
  F.Drop=0;F.Stop=TRUE;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  /* Admission failures do not program an alternate board/IP/rate. */
  D=Setup();D.Io.Physical[CrMdssCmdPhy]+=0x2000;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();D.Io.Bytes[CrMdssCmdPhy]=0x760;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();D.Plan.Config.Soc=0x8450;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();D.Plan.Config.Lanes=3;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();D.Plan.Config.Controller=1;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();++D.Plan.Config.LaneBitHz;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();++D.Plan.PhyTimings[13];
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();++D.Plan.PixelDivider;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();++D.Plan.VcoHz;
  assert(CmdPhyStart(&D)==CR_UNSUPPORTED&&!F.Writes);
  D=Setup();F.Reg[0x3c/4]=1;
  assert(CmdPhyStart(&D)==CR_NOT_READY&&!D.PhyTouched&&!F.Writes);
  D=Setup();assert(CmdPhyStart(&D)==CR_SUCCESS);D.ExternalOwned=TRUE;Before=F.Writes;
  assert(CmdPhyStop(&D)==CR_NOT_READY&&D.PhyTouched&&F.Writes==Before);
  D.ExternalOwned=FALSE;D.HostTouched=TRUE;
  assert(CmdPhyStop(&D)==CR_NOT_READY&&D.PhyTouched&&F.Writes==Before);
  D.HostTouched=FALSE;F.Stop=TRUE;assert(CmdPhyStop(&D)==CR_SUCCESS);Off();
  printf("PASS: upstream PHY/PLL traces; %u start + %u stop callbacks x2 fault modes; posted/drop/timeout/retry/admission\n",StartCalls,StopCalls);
  return 0;
}
