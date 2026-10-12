/* Source-oracle host setup and XML packet/DMA ownership tests. GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include "../Library/MdssDsiCmdCrLib/Internal.h"
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
#endif
typedef struct {UINT32 Type,Bytes,Delay;UINT8 Payload[25];} EXPECTED;
#include "packets.h"
void ReferenceHost(UINT32 *);
typedef struct {
  UINT32 R[0x400/4],Calls,Fail,Writes,Triggers,PublishCount,Drop;
  BOOLEAN Posted,StuckDone,StuckBusy,Lost,Reset,LoseOnDelay;
  UINT64 DelayTotal;
  UINT8 Command[256],Packets[64][256];UINT32 Length[64],Lp[64];
  UINT32 ResetValues[8],ResetCount;
} HW;
static CR_STATUS Step(HW *H){return ++H->Calls==H->Fail?CR_DEVICE_ERROR:CR_SUCCESS;}
static CR_STATUS Ready(VOID *C,CR_MDSS_CMD_WINDOW W){HW *H=C;assert(W==CrMdssCmdHost);return H->Lost?CR_NOT_READY:Step(H);}
static CR_STATUS Read(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 *V){HW *H=C;
  assert(W==CrMdssCmdHost&&!H->Lost&&!(O&3)&&O<0x400);CR_STATUS S=Step(H);
  if(S==CR_SUCCESS)*V=H->R[O/4];return S;}
static CR_STATUS Write(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V){HW *H=C;
  assert(W==CrMdssCmdHost&&!H->Lost&&!(O&3)&&O<0x400);CR_STATUS S=Step(H);
  if(S!=CR_SUCCESS&&!H->Posted)return S;
  ++H->Writes;if(O==H->Drop)return S;
  UINT32 Old=H->R[O/4];H->R[O/4]=V;
  if(O==0x110)H->R[O/4]=(V&0xaaaaaaaaU)|(Old&~V&0x55555555U);
  if(O==0x90){
    assert(V==1&&H->Triggers<64&&H->R[0x48/4]==0x81000000);
    UINT32 N=H->R[0x4c/4];assert(N<=256&&!(N&3));
    memcpy(H->Packets[H->Triggers],H->Command,N);H->Length[H->Triggers]=N;
    H->Lp[H->Triggers]=(H->R[0x3c/4]>>26)&1;++H->Triggers;
    if(!H->StuckDone)H->R[0x110/4]|=1;
    if(H->StuckBusy)H->R[2]|=2;
  }
  return S;
}
static CR_STATUS Fence(VOID *C){return Step(C);}
static CR_STATUS Delay(VOID *C,UINT32 Us){HW *H=C;assert(Us&&Us<=20000);H->DelayTotal+=Us;
  if(H->LoseOnDelay)H->Lost=TRUE;return Step(H);}
static CR_STATUS Publish(VOID *C,VOID *P,UINTN N){HW *H=C;assert(P==H->Command&&N<=256);++H->PublishCount;return Step(H);}
static CR_STATUS Reset(VOID *C,BOOLEAN High){HW *H=C;CR_STATUS S=Step(H);
  if(S==CR_SUCCESS||H->Posted){H->Reset=High;assert(H->ResetCount<8);H->ResetValues[H->ResetCount++]=High;}return S;}
static void Init(HW *H,CR_MDSS_CMD *D){memset(H,0,sizeof(*H));memset(D,0,sizeof(*D));H->Drop=~0U;
  CR_MDSS_CMD_CONFIG C;CrMdssCmdHdk8245Config(&C);CrMdssCmdR66451Profile(&D->Profile);D->Profile.Config=C;assert(CrMdssCmdBuildPlanProfile(&D->Profile,&D->Plan)==CR_SUCCESS);
  D->Io=(CR_MDSS_CMD_IO){.Context=H,.Ready=Ready,.Read32=Read,.Write32=Write,.Fence=Fence,.DelayUs=Delay,.PanelReset=Reset};
  D->Io.Bytes[CrMdssCmdHost]=0x400;
  D->Commands=(CR_MDSS_CMD_MEMORY){H,H->Command,sizeof(H->Command),0x81000000,Ready,Publish};
  D->Initialized=TRUE;D->PanelOwned=TRUE;
}
static void Packet(const HW *H,UINT32 I,const EXPECTED *E,BOOLEAN Lp){
  const UINT8 *P=H->Packets[I];BOOLEAN Long=E->Type==0x39;
  assert(P[2]==E->Type&&P[3]==(Long?0xc0:0x80)&&H->Lp[I]==Lp);
  if(Long){assert((P[0]|P[1]<<8)==E->Bytes);assert(!memcmp(P+4,E->Payload,E->Bytes));}
  else {assert(P[0]==E->Payload[0]&&P[1]==0&&H->Length[I]==4);}
}
int main(void){
  HW H;CR_MDSS_CMD D;UINT32 Ref[0x400/4]={0};Init(&H,&D);
  assert(CmdDelay(&D,CrMdssCmdHost,120000)==CR_SUCCESS&&H.DelayTotal==120000);
  Init(&H,&D);assert(CmdDelay(&D,CrMdssCmdHost,25001)==CR_SUCCESS&&H.DelayTotal==25001);
  for(UINT32 Us=20000;Us<=120000;Us+=100000){
    Init(&H,&D);H.LoseOnDelay=TRUE;
    assert(CmdDelay(&D,CrMdssCmdHost,Us)==CR_NOT_READY&&H.DelayTotal==20000);
  }
  Init(&H,&D);H.Fail=2;
  assert(CmdDelay(&D,CrMdssCmdHost,120000)==CR_DEVICE_ERROR&&H.DelayTotal==20000);
  Init(&H,&D);
  assert(CmdHostStart(&D)==CR_SUCCESS);ReferenceHost(Ref);
  const UINT32 Regs[]={4,0x84,0xc4,0xcc,0x11c,0x40,0x1b8,0x44,0xbc,0x2a8,0x2ac,0x58,0x60,0x5c,0x64};
  for(UINTN I=0;I<sizeof(Regs)/sizeof(Regs[0]);++I){
    if(H.R[Regs[I]/4]!=Ref[Regs[I]/4])fprintf(stderr,"offset %x got %x expected %x\n",Regs[I],H.R[Regs[I]/4],Ref[Regs[I]/4]);
    assert(H.R[Regs[I]/4]==Ref[Regs[I]/4]);}
  UINT32 StartCalls=H.Calls;assert(!(H.R[1]&2));
  assert(CmdPanelOn(&D)==CR_SUCCESS);assert(H.Triggers==21&&H.PublishCount==21);
  for(UINT32 I=0;I<20;++I)Packet(&H,I,&ExpectedOn[I],TRUE);
  assert(H.Packets[20][2]==0x0a&&H.Packets[20][0]==128&&H.Length[20]==132);
  assert(H.ResetCount==3&&H.ResetValues[0]==1&&H.ResetValues[1]==0&&H.ResetValues[2]==1);
  assert(H.DelayTotal>=190000);assert(CmdPanelOff(&D)==CR_SUCCESS);
  Packet(&H,21,&ExpectedOff[0],FALSE);Packet(&H,22,&ExpectedOff[1],FALSE);
  assert(!H.Reset&&!D.PanelTouched);assert(CmdHostStop(&D)==CR_SUCCESS&&!D.HostTouched);
  /* Every admission/read/write/fence failure, before and after posted writes.
   * No restart replay; successful Stop must physically idle and disable. */
  for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=StartCalls;++I){
    Init(&H,&D);H.Fail=I;H.Posted=Posted;assert(CmdHostStart(&D)!=CR_SUCCESS);
    H.Fail=0;assert(CmdHostStop(&D)==CR_SUCCESS);assert(!D.HostTouched&&!D.CommandInFlight);
  }
  Init(&H,&D);assert(CmdHostStart(&D)==CR_SUCCESS);H.Calls=0;
  const UINT8 Pps[128]={0x11};assert(CmdPacket(&D,0x0a,Pps,128,TRUE)==CR_SUCCESS);
  UINT32 PacketCalls=H.Calls;
  for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=PacketCalls;++I){
    Init(&H,&D);assert(CmdHostStart(&D)==CR_SUCCESS);H.Calls=0;H.Fail=I;H.Posted=Posted;
    assert(CmdPacket(&D,0x0a,Pps,128,TRUE)!=CR_SUCCESS);H.Fail=0;
    if(D.CommandInFlight)assert(CmdPacket(&D,0x0a,Pps,128,TRUE)==CR_NOT_READY);
    assert(CmdHostStop(&D)==CR_SUCCESS&&!D.CommandInFlight);
  }
  Init(&H,&D);assert(CmdHostStart(&D)==CR_SUCCESS);H.StuckDone=TRUE;H.R[0x110/4]=1;
  assert(CmdPacket(&D,0x0a,Pps,128,TRUE)==CR_TIMEOUT&&D.CommandInFlight);
  assert(CmdHostStop(&D)==CR_SUCCESS); /* idle read, not stale DONE, fences */
  Init(&H,&D);assert(CmdHostStart(&D)==CR_SUCCESS);H.StuckBusy=TRUE;
  assert(CmdPacket(&D,0x0a,Pps,128,TRUE)==CR_TIMEOUT&&D.CommandInFlight);
  assert(CmdHostStop(&D)==CR_TIMEOUT&&D.HostTouched&&D.CommandInFlight);
  H.R[2]=0;assert(CmdHostStop(&D)==CR_SUCCESS);
  Init(&H,&D);H.R[1]=3;assert(CmdHostStart(&D)==CR_NOT_READY&&!D.HostTouched&&!H.Writes);
  Init(&H,&D);assert(CmdHostStart(&D)==CR_SUCCESS);H.Lost=TRUE;
  assert(CmdHostStop(&D)==CR_NOT_READY&&D.HostTouched);H.Lost=FALSE;assert(CmdHostStop(&D)==CR_SUCCESS);
  CR_MDSS_CMD_CONFIG C;CR_MDSS_CMD_PLAN Plan;CrMdssCmdHdk8245Config(&C);
  assert(CrMdssCmdBuildPlan(&C,&Plan)==CR_SUCCESS&&Plan.MinimumLaneBitHz==307195200);
  assert(Plan.PacketBytes==540&&Plan.PacketsPerLine==2&&Plan.PayloadWidth==360);
  C.LaneBitHz=0;assert(CrMdssCmdBuildPlan(&C,&Plan)==CR_UNSUPPORTED);
  C.LaneBitHz=325733641;assert(CrMdssCmdBuildPlan(&C,&Plan)==CR_UNSUPPORTED);
  printf("PASS: independent Android host trace, 20+2 XML packets/PPS, %u host and %u DMA failure positions x2, stale/late DMA, lost ownership\n",StartCalls,PacketCalls);
  return 0;
}
