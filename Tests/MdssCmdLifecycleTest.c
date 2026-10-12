/* Actual whole command core under a deterministic MMIO/owner failure model.
 * Hardware register values are independently checked by per-unit oracles.
 * SPDX-License-Identifier: GPL-2.0-only */
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
typedef struct {
  UINT32 Dpu[0x83208/4],Host[0x400/4],Phy[0x7c0/4],Vbif[0x2008/4];
  UINT32 Calls,Fail;BOOLEAN Posted,IrqBusy,Irq,Clocks,Power,Reset;
  UINT32 ElapsedUs,FrameUntil,AutoRefreshReleases,SyncWrites;
  UINT8 Packets[64][256];UINT32 Length[64],Lp[64],PacketTime[64],Triggers;
  UINT32 ResetValues[8],ResetTime[8],ResetCount;
  BOOLEAN AutoRefreshActive,RepairExpected;
  CR_STATUS Failure;
  CR_MDSS_CMD *D;
} HW;
static HW H;static CR_MDSS_CMD D;
#ifndef MDSS_TEST_COMMAND_BYTES
#define MDSS_TEST_COMMAND_BYTES 256
#endif
static UINT8 Frame[4320*2340],Commands[MDSS_TEST_COMMAND_BYTES];
static CR_STATUS Step(HW *M){return ++M->Calls==M->Fail?M->Failure:CR_SUCCESS;}
static CR_STATUS Ready(VOID *C,CR_MDSS_CMD_WINDOW W){assert(W<CrMdssCmdWindows);return Step(C);}
static UINT32 *Reg(HW *M,CR_MDSS_CMD_WINDOW W,UINT32 O){assert(!(O&3));
  switch(W){case CrMdssCmdDpu:assert(O<sizeof(M->Dpu));return M->Dpu+O/4;
  case CrMdssCmdHost:assert(O<sizeof(M->Host));return M->Host+O/4;
  case CrMdssCmdPhy:assert(O<sizeof(M->Phy));return M->Phy+O/4;
  case CrMdssCmdVbif:assert(O<sizeof(M->Vbif));return M->Vbif+O/4;
  default:abort();}}
static CR_STATUS Read(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 *V){HW *M=C;CR_STATUS S=Step(M);
  if(S==CR_SUCCESS){
    if(W==CrMdssCmdDpu&&O==0x6aab0&&M->FrameUntil)*V=M->ElapsedUs<M->FrameUntil?512:0;
    else if(W==CrMdssCmdDpu&&O==0x34c){assert(M->Dpu[0x418/4]==0x405&&M->Dpu[0x348/4]==0x7001);*V=M->AutoRefreshActive?0x80:0;}
    else *V=*Reg(M,W,O);
  }return S;}
static CR_STATUS Write(VOID *C,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V){HW *M=C;CR_STATUS S=Step(M);
  if(S!=CR_SUCCESS&&!M->Posted)return S;UINT32 *R=Reg(M,W,O),Old=*R;*R=V;
  if(W==CrMdssCmdHost){
    if(O==0x110)*R=(V&0xaaaaaaaaU)|(Old&~V&0x55555555U);
    if(O==0x90){
      UINT32 N=M->Host[0x4c/4];assert(V==1&&M->Triggers<64&&N<=sizeof(Commands));
      memcpy(M->Packets[M->Triggers],Commands,N);M->Length[M->Triggers]=N;
      M->Lp[M->Triggers]=(M->Host[0x3c/4]>>26)&1;M->PacketTime[M->Triggers]=M->ElapsedUs;
      ++M->Triggers;M->Host[0x110/4]|=1;
    }
  }
  if(W==CrMdssCmdPhy&&O==0x3c)M->Phy[0x6b0/4]=V&1;
  if(W==CrMdssCmdVbif&&O==0x200){assert(M->Dpu[0x2ac/4]&0x100);M->Vbif[0x204/4]=V&2;}
  if(W==CrMdssCmdDpu){
    if(O==0x24038)assert(M->D->IrqOwned);
    if((O==0x6aa84&&!V)||(O==0x1030&&(V&1)))assert(!M->AutoRefreshActive&&M->ElapsedUs>=M->FrameUntil);
    if(O==0x6aa90&&M->RepairExpected){++M->SyncWrites;if(M->AutoRefreshReleases&&!--M->AutoRefreshReleases)M->AutoRefreshActive=FALSE;}
    if(O==0x18)M->Dpu[0x14/4]&=~V;
    if(O==0x6d808)M->Dpu[0x6d804/4]&=~V;
    if(O==0x1030){*R=0;M->Dpu[0x1018/4]=0;}
    if(O==0x6aab4&&(V>>31))M->Dpu[0x1018/4]=0;
  }
  return S;
}
static CR_STATUS Fence(VOID *C){return Step(C);}
static CR_STATUS Delay(VOID *C,UINT32 Us){HW *M=C;assert(Us<=120000);CR_STATUS S=Step(C);if(S==CR_SUCCESS||M->Posted)M->ElapsedUs+=Us;return S;}
static CR_STATUS Publish(VOID *C,VOID *Cpu,UINTN Bytes){assert(Bytes&&
  ((Cpu>=(VOID*)Frame&&(UINT8*)Cpu+Bytes<=Frame+sizeof(Frame))||Cpu==(VOID*)Commands));return Step(C);}
static CR_STATUS Reset(VOID *C,BOOLEAN On){HW *M=C;assert(M->Power);CR_STATUS S=Step(M);
  if(S==CR_SUCCESS||M->Posted){M->Reset=On;assert(M->ResetCount<8);
    M->ResetValues[M->ResetCount]=On;M->ResetTime[M->ResetCount++]=M->ElapsedUs;}return S;}
static CR_STATUS Power(VOID *C,BOOLEAN On,BOOLEAN *Owned){HW *M=C;CR_STATUS S=Step(M);
  if(!On)assert(!M->D->PhyTouched&&!M->D->HostTouched&&!M->D->DpuTouched&&!M->D->CommandInFlight&&!M->Reset);
  if(S==CR_SUCCESS||M->Posted){M->Power=On;*Owned=On;}return S;}
static CR_STATUS Clocks(VOID *C,CONST CR_MDSS_CMD_PLAN *P,BOOLEAN On,BOOLEAN *Owned){HW *M=C;CR_STATUS S=Step(M);assert(P->ByteHz==40887068);
  if(!On)assert(!M->D->HostTouched&&!M->D->DpuTouched&&!M->D->DscTouched&&!M->D->CommandInFlight);
  if(S==CR_SUCCESS||M->Posted){M->Clocks=On;*Owned=On;}return S;}
static CR_STATUS Events(VOID *C,BOOLEAN On,BOOLEAN *Owned){HW *M=C;CR_STATUS S=Step(M);
  if(!On&&M->IrqBusy)return CR_NOT_READY;
  if(S==CR_SUCCESS||M->Posted){M->Irq=On;*Owned=On;}return S;}
static void Init(void){memset(&H,0,sizeof(H));memset(&D,0,sizeof(D));H.D=&D;H.Phy[0x140/4]=1;H.Failure=CR_DEVICE_ERROR;
  CR_MDSS_CMD_IO Io={.Context=&H,.Physical={0xae01000,0xae94000,0xae94400,0xaeb0000},
    .Bytes={0x83208,0x400,0x7c0,0x2008},.Ready=Ready,.Read32=Read,.Write32=Write,.Fence=Fence,.DelayUs=Delay,
    .ExternalClocks=Clocks,.PanelPower=Power,.PanelReset=Reset,.Events=Events};
  CR_MDSS_CMD_CONFIG C;CrMdssCmdHdk8245Config(&C);
  CR_MDSS_CMD_MEMORY F={&H,Frame,sizeof(Frame),0x82000000,Ready,Publish};
  CR_MDSS_CMD_MEMORY B={&H,Commands,sizeof(Commands),0x83000000,Ready,Publish};
  assert(CrMdssCmdInit(&D,&Io,&C,&F,&B)==CR_SUCCESS);}
static void FirstFrame(void){UINT32 E=0;H.Dpu[0x14/4]|=1U<<8;
  assert(CrMdssCmdHandleIrq(&D,&E)==CR_SUCCESS&&(E&CR_MDSS_CMD_FRAME_DONE));
  assert(CrMdssCmdStart(&D)==CR_SUCCESS&&D.Active);}
static void Finished(void){assert(!H.Power&&!H.Clocks&&!H.Irq&&!H.Reset);
  assert(CrMdssCmdUninit(&D)==CR_SUCCESS&&!D.Initialized);}
static void Queued(void){H.FrameUntil=H.ElapsedUs+6000;H.AutoRefreshActive=H.RepairExpected=TRUE;
  H.AutoRefreshReleases=3;H.Dpu[0x418/4]=0x12;H.Dpu[0x348/4]=0x23;}
static void QueuedStop(void){
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);FirstFrame();Queued();H.Calls=0;
  assert(CrMdssCmdStop(&D)==CR_SUCCESS&&H.SyncWrites==4);UINT32 Count=H.Calls;Finished();
  for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=Count;++I){
    Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);FirstFrame();Queued();H.Calls=0;H.Fail=I;H.Posted=Posted;
    H.Failure=Kind?1:CR_DEVICE_ERROR;assert(CrMdssCmdStop(&D)!=CR_SUCCESS&&D.CleanupPending);
    assert(CrMdssCmdUninit(&D)==CR_NOT_READY);H.Fail=0;H.AutoRefreshActive=FALSE;H.FrameUntil=0;
    assert(CrMdssCmdStop(&D)==CR_SUCCESS&&!D.DpuDebugTouched);
    assert(H.Dpu[0x418/4]==0x12&&H.Dpu[0x348/4]==0x23);Finished();
  }
  printf("PASS: actual queued full lifecycle stop %u callback positions x4, retained selectors and provider lifetimes\n",Count);
}
static void ProfileTests(void){
  CR_MDSS_CMD_PROFILE P;CrMdssCmdR66451Profile(&P);
  P.Config.Width=P.Config.Dsc.PictureWidth=720;P.Config.Height=P.Config.Dsc.PictureHeight=1280;
  P.Config.Stride=2880;P.Config.Dsc.SliceWidth=360;
  P.Config.HSync=2;P.Config.HBackPorch=10;P.Config.HFrontPorch=12;
  P.Config.VSync=2;P.Config.VBackPorch=5;P.Config.VFrontPorch=8;
  P.ResetCount=2;P.Reset[0]=(CR_MDSS_CMD_RESET){FALSE,7};P.Reset[1]=(CR_MDSS_CMD_RESET){TRUE,11};
  P.OnCount=3;P.On[0]=(CR_MDSS_CMD_PACKET){0x15,FALSE,2,33,{0xa1,0x5b}};
  P.On[1]=(CR_MDSS_CMD_PACKET){0x39,TRUE,3,44,{0xca,0x24,0x31}};
  P.On[2]=(CR_MDSS_CMD_PACKET){0x05,TRUE,1,55,{0x11}};
  P.OffCount=1;P.Off[0]=(CR_MDSS_CMD_PACKET){0x05,FALSE,1,66,{0x28}};
  P.OffResetDelayUs=13;P.BrightnessCommand=0x52;P.BrightnessMsbFirst=FALSE;
  Init();CR_MDSS_CMD_IO Io=D.Io;CR_MDSS_CMD_MEMORY F=D.Frame,B=D.Commands;
  Finished();assert(CrMdssCmdInitProfile(&D,&Io,&P,&F,&B)==CR_SUCCESS);
  /* Destroy the caller's entire descriptor before its packets/reset/timings
   * are consumed. The retained provider data must have been captured by value. */
  memset(&P,0xff,sizeof(P));
  assert(CrMdssCmdStart(&D)==CR_NOT_READY&&!D.Active&&D.Starting);
  assert(H.Triggers==4&&H.ResetCount==2&&!H.ResetValues[0]&&H.ResetValues[1]);
  assert(H.ResetTime[1]-H.ResetTime[0]==7&&H.PacketTime[0]-H.ResetTime[1]==11);
  assert(H.PacketTime[1]-H.PacketTime[0]==33&&H.PacketTime[2]-H.PacketTime[1]==44);
  assert(H.PacketTime[3]-H.PacketTime[2]==55);
  const UINT8 Short[]={0xa1,0x5b,0x15,0x80};
  const UINT8 Long[]={3,0,0x39,0xc0,0xca,0x24,0x31,0xff};
  assert(H.Length[0]==4&&!memcmp(H.Packets[0],Short,sizeof(Short))&&!H.Lp[0]);
  assert(H.Length[1]==8&&!memcmp(H.Packets[1],Long,sizeof(Long))&&H.Lp[1]);
  /* DSC PPS picture height/width and slice height/width, network byte order. */
  const UINT8 PpsGeometry[]={5,0,2,0xd0,0,20,1,0x68};
  assert(H.Packets[3][2]==0x0a&&H.Length[3]==132&&!memcmp(H.Packets[3]+10,PpsGeometry,8));
  assert(H.Host[0x2ac/4]==360&&H.Host[0x58/4]==((361U<<16)|0x39));
  assert(H.Host[0x5c/4]==((1280U<<16)|240));
  assert(H.Dpu[0x24000/4]==((1280U<<16)|720)&&H.Dpu[0x24024/4]==2880);
  assert(H.Dpu[0x44004/4]==((1280U<<16)|720)&&H.Dpu[0x6aa88/4]==0x800f7);
  assert(H.Dpu[0x6aa90/4]==1285&&H.Dpu[0x6aa94/4]==1280);
  UINT32 Calls=H.Calls;assert(CrMdssCmdStart(&D)==CR_NOT_READY&&H.Calls==Calls);
  FirstFrame();assert(CrMdssCmdSetBrightness(&D,0xabc)==CR_SUCCESS);
  const UINT8 Bright[]={3,0,0x39,0xc0,0x52,0xbc,0x0a,0xff};
  assert(H.Triggers==5&&!memcmp(H.Packets[4],Bright,sizeof(Bright)));
  Calls=H.Calls;assert(CrMdssCmdSetBrightness(&D,4096)==CR_INVALID_PARAMETER&&H.Calls==Calls);
  assert(CrMdssCmdStop(&D)==CR_SUCCESS&&H.Triggers==6&&H.ResetCount==3&&!H.ResetValues[2]);
  assert(H.Packets[5][0]==0x28&&H.Packets[5][2]==0x05&&!H.Lp[5]);Finished();
  /* Unsupported hardware, analogue timing and malformed bounded programs
   * must be rejected before any provider callback or owner mutation. */
  for(UINT32 I=0;I<13;++I){
    CrMdssCmdR66451Profile(&P);CR_STATUS Expected=CR_UNSUPPORTED;
    switch(I){
    case 0:P.Hardware.DpuMajor=8;break;
    case 1:P.Hardware.Flags|=8;break;
    case 2:P.Config.LaneBitHz--;break;
    case 3:P.PhyTimings[0]++;break;
    case 4:P.Config.Width=0xffffffffU;Expected=CR_INVALID_PARAMETER;break;
    case 5:P.Config.VFrontPorch=0xffffffffU;break;
    case 6:P.OnCount=CR_MDSS_CMD_ON_MAX+1;Expected=CR_INVALID_PARAMETER;break;
    case 7:P.On[0].Bytes=CR_MDSS_CMD_PAYLOAD_MAX+1;Expected=CR_INVALID_PARAMETER;break;
    case 8:P.Reset[P.ResetCount-1].High=FALSE;break;
    case 9:P.On[0].Type=0x0a;break;
    case 10:P.On[0].Type=0x15;P.On[0].Bytes=3;Expected=CR_INVALID_PARAMETER;break;
    case 11:P.BrightnessBytes=1;Expected=CR_INVALID_PARAMETER;break;
    default:P.Reset[0].DelayUs=120001;Expected=CR_INVALID_PARAMETER;break;
    }
    CR_MDSS_CMD Saved=D;Calls=H.Calls;
    assert(CrMdssCmdInitProfile(&D,&Io,&P,&F,&B)==Expected);
    assert(!memcmp(&D,&Saved,sizeof(D))&&H.Calls==Calls);
  }
  CrMdssCmdR66451Profile(&P);P.Config.Width=P.Config.Dsc.PictureWidth=720;
  P.Config.Height=P.Config.Dsc.PictureHeight=1280;P.Config.Stride=2880;P.Config.Dsc.SliceWidth=360;
  CR_MDSS_CMD_PLAN Plan;
  assert(CrMdssCmdBuildPlanProfile(&P,&Plan)==CR_SUCCESS&&Plan.MinimumLaneBitHz==112742400);
  assert(CrMdssCmdBuildPlan(&P.Config,&Plan)==CR_UNSUPPORTED);
  puts("PASS: provider profile on actual PHY/host/panel/DSC/DPU lifecycle, by-value capture, pre-effect rejection and first-frame gate");
}
int main(void){
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY&&D.Starting&&!D.Active&&!D.CleanupPending);
  UINT32 StartCalls=H.Calls;assert(CrMdssCmdUninit(&D)==CR_NOT_READY);
  assert(CrMdssCmdStart(&D)==CR_NOT_READY&&H.Calls==StartCalls);
  FirstFrame();assert(CrMdssCmdPublish(&D,0,sizeof(Frame))==CR_SUCCESS);
  assert(CrMdssCmdSetBrightness(&D,4095)==CR_SUCCESS);
  H.Calls=0;assert(CrMdssCmdStop(&D)==CR_SUCCESS);UINT32 StopCalls=H.Calls;Finished();
  /* Fault every callback across actual PHY+host+panel+DSC+DPU composition. */
  for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=StartCalls;++I){
    Init();H.Fail=I;H.Posted=Posted;H.Failure=Kind?1:CR_DEVICE_ERROR;CR_STATUS S=CrMdssCmdStart(&D);
    assert(S!=CR_SUCCESS&&!D.Active);H.Fail=0;
    if(D.CleanupPending)assert(CrMdssCmdStart(&D)==CR_NOT_READY);
    assert(CrMdssCmdStop(&D)==CR_SUCCESS);Finished();
  }
  for(UINT32 Kind=0;Kind<2;++Kind)for(UINT32 Posted=0;Posted<2;++Posted)for(UINT32 I=1;I<=StopCalls;++I){
    Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);FirstFrame();H.Calls=0;H.Fail=I;H.Posted=Posted;
    H.Failure=Kind?1:CR_DEVICE_ERROR;assert(CrMdssCmdStop(&D)!=CR_SUCCESS&&D.CleanupPending);
    assert(CrMdssCmdUninit(&D)==CR_NOT_READY);H.Fail=0;
    assert(CrMdssCmdStop(&D)==CR_SUCCESS);Finished();
  }
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);FirstFrame();H.IrqBusy=TRUE;
  assert(CrMdssCmdStop(&D)==CR_NOT_READY&&D.IrqOwned&&D.FrameInFlight&&H.Clocks&&H.Power);
  assert(CrMdssCmdUninit(&D)==CR_NOT_READY);H.IrqBusy=FALSE;
  assert(CrMdssCmdStop(&D)==CR_SUCCESS);Finished();
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);UINT32 E;
  H.Dpu[0x14/4]|=1U<<26;assert(CrMdssCmdHandleIrq(&D,&E)==CR_DEVICE_ERROR&&D.CleanupPending);
  assert(CrMdssCmdStart(&D)==CR_NOT_READY);assert(CrMdssCmdStop(&D)==CR_SUCCESS);Finished();
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);H.Dpu[0x14/4]|=1U<<8;
  assert(CrMdssCmdHandleIrq(&D,&E)==CR_SUCCESS&&D.FirstFrameDone);
  H.Fail=H.Calls+1;assert(CrMdssCmdStart(&D)==CR_DEVICE_ERROR&&!D.Active&&D.CleanupPending);
  H.Fail=0;assert(CrMdssCmdStop(&D)==CR_SUCCESS);Finished();
  Init();assert(CrMdssCmdStart(&D)==CR_NOT_READY);D.FrameSerial=123;
  CR_MDSS_CMD Saved=D;UINT32 Calls=H.Calls;
  assert(CrMdssCmdHandleIrq(&D,(UINT32*)&D.FrameSerial)==CR_INVALID_PARAMETER);
  assert(!memcmp(&D,&Saved,sizeof(D))&&Calls==H.Calls);
  assert(CrMdssCmdHandleIrq(&D,(UINT32*)Commands)==CR_INVALID_PARAMETER&&Calls==H.Calls);
  assert(CrMdssCmdStop(&D)==CR_SUCCESS);Finished();
  printf("PASS: actual whole command lifecycle, %u start / %u stop failure positions x4, deferred IRQ drain, real first-frame gate, underrun stop\n",StartCalls,StopCalls);
  QueuedStop();
  ProfileTests();
  return 0;
}
