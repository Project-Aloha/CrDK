/* Actual extended host/PHY core against independently compiled source clocks,
 * timing oracle and hostile retained IO. SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#undef NULL
#include <Library/MdssDsiCrLib.h>
#include <Library/MdssDualCrLib.h>
#ifndef _KERNEL_MODE
VOID *EFIAPI SetMem(VOID *P,UINTN N,UINT8 V){return memset(P,V,N);}
INTN EFIAPI CompareMem(CONST VOID *A,CONST VOID *B,UINTN N){return memcmp(A,B,N);}
VOID *EFIAPI CopyMem(VOID *D,CONST VOID *S,UINTN N){return memmove(D,S,N);}
#endif
void ReferenceTiming(UINT64,BOOLEAN,UINT32 *);
void ReferenceClock(UINT64,BOOLEAN,UINT32,UINT64 *,UINT32 *,UINT32 *,UINT32 *,UINT64 *);
void ReferenceDpuTiming(CONST CR_MDSS_DSI_PLAN *,UINT32,UINT32 *);
void ReferenceDpuSplit(BOOLEAN,UINT32 *);
typedef struct {
 UINT32 R[CrMdssExtWindows][0x800/4],Calls,Fail,Writes,StopCalls;
 UINT32 DropWindow,DropOffset,OnOrder[2],OnCount,OffOrder[2],OffCount,RefgenWrites;
 CR_STATUS Failure;BOOLEAN Lost[CrMdssExtWindows],Clock[2],CommitFailure;
 BOOLEAN StopChecking;
 UINT8 Command[256];UINT32 PacketTriggers;BOOLEAN NeverComplete,CommandError;
 BOOLEAN PanelOwned,StuckReset,StuckTiming,StuckFlush;
} HW;
static UINT32 mDpu[0x90000/4];
static CR_STATUS Step(HW *H){++H->Calls;return H->Calls==H->Fail?H->Failure:CR_SUCCESS;}
static CR_STATUS Ready(VOID *C,CR_MDSS_EXT_WINDOW W){HW *H=C;
 assert(W<CrMdssExtWindows);return H->Lost[W]?CR_NOT_READY:Step(H);}
static CR_STATUS Read(VOID *C,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 *V){HW *H=C;
 assert(!H->Lost[W]&&!(O&3)&&O<(W==CrMdssExtDpu?0x90000U:0x800U));CR_STATUS S=Step(H);
 if(S==CR_SUCCESS){
  if(W==CrMdssExtDpu){*V=mDpu[O/4];
   if((O==0x350ac||O==0x360ac)&&mDpu[(O-0xac)/4]){*V=++mDpu[O/4];}}
  else *V=H->R[W][O/4];
 }return S;}
static CR_STATUS Write(VOID *C,CR_MDSS_EXT_WINDOW W,UINT32 O,UINT32 V){HW *H=C;
 assert(!H->Lost[W]&&!(O&3)&&O<(W==CrMdssExtDpu?0x90000U:0x800U));CR_STATUS S=Step(H);if(S!=CR_SUCCESS)return S;
 if(W==CrMdssExtDpu){
  assert(O!=0x1512c&&O!=0x15130);++H->Writes;
  if(W!=H->DropWindow||O!=H->DropOffset)mDpu[O/4]=V;
  if(O==0x35000||O==0x36000){if(V||!H->StuckTiming)mDpu[(O+0x26c)/4]=V&1;}
  if(O==0x1501c&&!H->StuckFlush)mDpu[0x15018/4]=0;
  if(O==0x15030&&!H->StuckReset)mDpu[O/4]=0;
  return CR_SUCCESS;
 }
 if(H->StopChecking&&W>=CrMdssExtPhy0&&O==0x3c&&!(V&1)){
  assert(!(H->R[CrMdssExtDsi0][1]&7)&&!(H->R[CrMdssExtDsi1][1]&7));
  assert(!H->Clock[0]&&!H->Clock[1]);
 }
 UINT32 Old=H->R[W][O/4];
 if(W>=CrMdssExtPhy0&&O==0x1ac)++H->RefgenWrites;
 ++H->Writes;if(W!=H->DropWindow||O!=H->DropOffset)H->R[W][O/4]=V;
 if(W==CrMdssExtDsi0||W==CrMdssExtDsi1){
  if(O==0x110)H->R[W][O/4]=(V&0xaaaaaaaaU)|(Old&~V&0x55555555U);
  if(O==0x90){++H->PacketTriggers;H->R[W][0x110/4]|=(H->NeverComplete?0:1)|
    (H->CommandError?1U<<24:0);}
  if(O==0x118&&V==1){H->R[W][8/4]=0;H->R[W][0x110/4]=0;}
 }
 if(W>=CrMdssExtPhy0&&O==0x3c)H->R[W][0x6b0/4]=V&1;
 return CR_SUCCESS;
}
static CR_STATUS Fence(VOID *C){return Step(C);}
static CR_STATUS Delay(VOID *C,UINT32 Us){assert(Us<=20000);return Step(C);}
static CR_STATUS CommandReady(VOID *C,UINT32 Id){HW *H=C;assert(Id<2);return Step(H);}
static CR_STATUS Publish(VOID *C,VOID *Cpu,UINTN Bytes){HW *H=C;
 assert(Cpu==H->Command&&Bytes<=sizeof(H->Command));return Step(H);}
static CR_STATUS Clocks(VOID *C,UINT32 Id,CONST CR_MDSS_DSI_PLAN *P,BOOLEAN On,BOOLEAN *Owned){
 HW *H=C;assert(Id<2&&P->ByteHz&&P->PixelDivider);CR_STATUS S=Step(H);
 if(S==CR_SUCCESS||(On&&H->CommitFailure)){
  if(On){H->Clock[Id]=TRUE;*Owned=TRUE;H->OnOrder[H->OnCount++%2]=Id;}
  else{assert(!(H->R[CrMdssExtDsi0][1]&7)&&!(H->R[CrMdssExtDsi1][1]&7));
   H->Clock[Id]=FALSE;*Owned=FALSE;H->OffOrder[H->OffCount++%2]=Id;}
 }
 return S;
}
static CR_MDSS_DSI_MODE Mode(BOOLEAN Cphy,UINT32 Count,UINT32 Lanes){
 CR_MDSS_DSI_MODE M={0};M.Revision=1;M.Controllers=Count;M.PhyType=Cphy?CrMdssCphy:CrMdssDphy;
 M.PixelHz=148500000;M.ContinuousClock=TRUE;
 M.Raster=(CR_MDSS_MODE){.Soc=0x8450,.Width=1920,.Height=1080,.Stride=7680,
  .HSync=44,.HBackPorch=148,.HFrontPorch=88,.VSync=5,.VBackPorch=36,.VFrontPorch=4,
  .RefreshQ16=60U<<16,.Lanes=Lanes};
 M.LaneMap[0]=1;M.LaneMap[1]=2;M.LaneMap[2]=4;M.LaneMap[3]=8;return M;
}
static CR_MDSS_EXT_IO Io(HW *H){CR_MDSS_EXT_IO I={.Context=H,.Ready=Ready,.Read32=Read,
 .Write32=Write,.Fence=Fence,.DelayUs=Delay,.ExternalClocks=Clocks};
 I.Physical[CrMdssExtDsi0]=0xae94000;I.Physical[CrMdssExtDsi1]=0xae96000;
 I.Physical[CrMdssExtPhy0]=0xae94400;I.Physical[CrMdssExtPhy1]=0xae96400;
 I.Bytes[CrMdssExtDsi0]=I.Bytes[CrMdssExtDsi1]=0x400;
 I.Bytes[CrMdssExtPhy0]=I.Bytes[CrMdssExtPhy1]=0x800;
 I.Physical[CrMdssExtDpu]=0xae01000;I.Bytes[CrMdssExtDpu]=0x90000;return I;}
static void Reset(HW *H){memset(H,0,sizeof(*H));H->DropWindow=0xffffffff;H->DropOffset=0xffffffff;
 H->R[CrMdssExtPhy0][0x140/4]=H->R[CrMdssExtPhy1][0x140/4]=1;memset(mDpu,0,sizeof(mDpu));}
static void Init(HW *H,CR_MDSS_DSI *D,BOOLEAN Cphy,UINT32 Count){
 CR_MDSS_EXT_IO I=Io(H);CR_MDSS_DSI_MODE M=Mode(Cphy,Count,Cphy?2:4);
 UINT32 Calls=H->Calls;assert(CrMdssDsiInit(D,&I,&M)==CR_SUCCESS&&H->Calls==Calls);
}
static void Pure(void){
 CR_MDSS_DSI_MODE M;CR_MDSS_DSI_PLAN P,Old;UINT32 C,N,F,T[14],Pll,Phy,Div,Vectors=0;UINT64 Vco,Source;
 for(C=0;C<2;++C)for(N=1;N<=2;++N)for(UINT32 L=1;L<=(C?3U:4U);++L)
 for(F=16000001;F<=300000000;F+=7100000){
  M=Mode(C,N,L);M.PixelHz=F;M.Raster.RefreshQ16=(UINT32)((UINT64)F*65536/(2200*1125));
  CR_STATUS S=CrMdssDsiBuildPlan(&M,&P);if(S!=CR_SUCCESS)continue;
  ReferenceTiming(P.SymbolHz,C,T);assert(!memcmp(T,P.Timings,sizeof(T)));
  ReferenceClock(P.ByteHz,C,L,&Vco,&Pll,&Phy,&Div,&Source);
  assert(Vco==P.VcoHz&&Pll==P.PllPostDivider&&Phy==P.PhyPostDivider&&Div==P.PixelDivider);
  assert(Source==P.PixelSourceHz&&P.ByteInterfaceDivider==(C?1U:2U));
  CR_CLOCK_DSI_PLAN Clock;assert(CrMdssDsiClockPlan(&P,0,&Clock)==CR_SUCCESS);
  UINT64 Actual=Source*P.PixelM/P.PixelN;
  assert((Actual>P.PixelHz?Actual-P.PixelHz:P.PixelHz-Actual)<8);++Vectors;
 }
 M=Mode(TRUE,2,2);assert(CrMdssDsiBuildPlan(&M,&P)==CR_SUCCESS);
 assert(P.PixelM==2&&P.PixelN==3&&P.PixelMux==3&&P.Link[1].Controller==1);
 assert(P.ByteHz==55687500&&P.SymbolHz==389812500&&P.PixelHz==74250000);
 memset(&P,0xa5,sizeof(P));Old=P;M.PhyType=(CR_MDSS_PHY_TYPE)-1;
 assert(CrMdssDsiBuildPlan(&M,&P)==CR_INVALID_PARAMETER&&!memcmp(&P,&Old,sizeof(P)));
 M=Mode(TRUE,1,4);assert(CrMdssDsiBuildPlan(&M,&P)==CR_INVALID_PARAMETER);
 M=Mode(FALSE,2,4);M.Raster.HFrontPorch|=1;assert(CrMdssDsiBuildPlan(&M,&P)==CR_UNSUPPORTED);
 M=Mode(TRUE,2,2);M.LaneMap[3]=4;assert(CrMdssDsiBuildPlan(&M,&P)==CR_INVALID_PARAMETER);
 M=Mode(TRUE,2,2);M.FirstController=1;assert(CrMdssDsiBuildPlan(&M,&P)==CR_INVALID_PARAMETER);
 M=Mode(TRUE,2,2);M.PixelHz/=2;assert(CrMdssDsiBuildPlan(&M,&P)==CR_INVALID_PARAMETER);
 M=Mode(TRUE,2,2);M.Raster.InvertHsync=TRUE;assert(CrMdssDsiBuildPlan(&M,&P)==CR_UNSUPPORTED);
 M=Mode(TRUE,2,2);M.Dsc=TRUE;M.SlicesPerPacket=1;
 CR_MDSS_DSC_PARAMS Q={.Revision=1,.MinorVersion=2,.BitsPerComponent=8,.BppQ4=128,
  .PictureWidth=1920,.PictureHeight=1080,.SliceWidth=480,.SliceHeight=8,.BlockPrediction=TRUE};
 assert(CrMdssDscBuild(&Q,&M.Compression)==CR_SUCCESS);
 assert(CrMdssDsiBuildPlan(&M,&P)==CR_SUCCESS);
 assert(P.PayloadWidth==320&&P.PacketBytes==480&&P.PacketsPerLine==2&&P.EndOfLineBytes==0);
 M.SlicesPerPacket=3;assert(CrMdssDsiBuildPlan(&M,&P)==CR_UNSUPPORTED);
 printf("DPHY/CPHY source clock and timing oracle: %u vectors\n",Vectors);
}
static void Happy(void){HW H;CR_MDSS_DSI D;for(UINT32 C=0;C<2;++C)for(UINT32 N=1;N<=2;++N){
 Reset(&H);Init(&H,&D,C,N);assert(CrMdssDsiPrepare(&D)==CR_SUCCESS&&D.Prepared);
 for(UINT32 I=0;I<N;++I){UINT32 Host=CrMdssExtDsi0+I,Phy=CrMdssExtPhy0+I;
  assert(H.R[Host][0x2d8/4]==C&&H.R[Host][0x330/4]==0);
  assert(H.R[Host][0xbc/4]==0x3fd08&&H.R[Host][0x1e4/4]==1);
  assert((H.R[Phy][0x14/4]&15)==((C?3U:1U)|(I?4U:0U)));
  assert((H.R[Phy][0x10/4]&15)==(I?1U:D.Plan.PhyPostDivider));
  assert(H.R[Phy][0x3c/4]==(I?0U:1U));
 }
 assert(CrMdssDsiEnable(&D)==CR_SUCCESS&&D.Active);
 UINT32 Writes=H.Writes;H.Lost[CrMdssExtPhy0+N-1]=TRUE;
 assert(CrMdssDsiPrepare(&D)==CR_NOT_READY&&CrMdssDsiEnable(&D)==CR_NOT_READY&&H.Writes==Writes);
 H.Lost[CrMdssExtPhy0+N-1]=FALSE;H.StopChecking=TRUE;
 assert(CrMdssDsiStop(&D)==CR_SUCCESS&&!D.Active&&!D.CleanupPending);
 assert(!H.Clock[0]&&!H.Clock[1]&&H.OffOrder[0]==N-1);
 assert(CrMdssDsiUninit(&D)==CR_SUCCESS&&!D.Initialized);
 }
 Reset(&H);CR_MDSS_DSI_MODE M=Mode(FALSE,1,4);M.FirstController=M.Raster.Controller=1;
 M.ContinuousClock=FALSE;CR_MDSS_EXT_IO I=Io(&H);assert(CrMdssDsiInit(&D,&I,&M)==CR_SUCCESS);
 assert(CrMdssDsiPrepare(&D)==CR_SUCCESS&&!H.Clock[0]&&H.Clock[1]);
 assert(!(H.R[CrMdssExtDsi1][0xac/4]&(1U<<28)));assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
}
static void Faults(void){HW H;CR_MDSS_DSI D;UINT32 Prepare,Enable,Stop;UINT32 Total=0;
 for(UINT32 C=0;C<2;++C){
  Reset(&H);Init(&H,&D,C,2);assert(CrMdssDsiPrepare(&D)==CR_SUCCESS);Prepare=H.Calls;H.Calls=0;
  assert(CrMdssDsiEnable(&D)==CR_SUCCESS);Enable=H.Calls;H.Calls=0;H.StopChecking=TRUE;
  assert(CrMdssDsiStop(&D)==CR_SUCCESS);Stop=H.Calls;
  for(UINT32 Stage=0;Stage<3;++Stage)for(UINT32 Warning=0;Warning<2;++Warning)
  for(UINT32 F=1;F<=(Stage==0?Prepare:Stage==1?Enable:Stop);++F){
   Reset(&H);Init(&H,&D,C,2);if(Stage>0)assert(CrMdssDsiPrepare(&D)==CR_SUCCESS);
   if(Stage>1)assert(CrMdssDsiEnable(&D)==CR_SUCCESS);
   H.Calls=0;H.Fail=F;H.Failure=Warning?1:CR_DEVICE_ERROR;H.CommitFailure=TRUE;
   CR_STATUS S=Stage==0?CrMdssDsiPrepare(&D):Stage==1?CrMdssDsiEnable(&D):CrMdssDsiStop(&D);
   assert(S!=CR_SUCCESS);assert(D.CleanupPending);
   assert(CrMdssDsiPrepare(&D)==CR_NOT_READY);H.Fail=0;H.StopChecking=TRUE;
   assert(CrMdssDsiUninit(&D)==CR_SUCCESS&&!D.Initialized&&!H.Clock[0]&&!H.Clock[1]);++Total;
  }
 }
 Reset(&H);Init(&H,&D,TRUE,2);H.DropWindow=CrMdssExtPhy1;H.DropOffset=0x1c;
 assert(CrMdssDsiPrepare(&D)==CR_TIMEOUT&&D.PhyTouched[0]&&D.PhyTouched[1]);
 H.DropWindow=0xffffffff;assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 Reset(&H);Init(&H,&D,FALSE,2);H.R[CrMdssExtPhy1][0x3c/4]=1;
 assert(CrMdssDsiPrepare(&D)==CR_BUSY&&H.Writes==0);
 H.R[CrMdssExtPhy1][0x3c/4]=0;assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 Reset(&H);Init(&H,&D,TRUE,2);assert(CrMdssDsiPrepare(&D)==CR_SUCCESS);
 H.Lost[CrMdssExtPhy1]=TRUE;UINT32 Before=H.Writes;
 assert(CrMdssDsiStop(&D)==CR_NOT_READY&&D.CleanupPending&&D.PhyTouched[1]);
 assert(H.Writes>=Before);Before=H.Writes;assert(CrMdssDsiStop(&D)==CR_NOT_READY&&H.Writes==Before);
 H.Lost[CrMdssExtPhy1]=FALSE;assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 printf("Actual DSI/PHY retained callback failures: %u phases\n",Total);
}
static void CommandInit(HW *H,CR_MDSS_DSI *D){
 CR_MDSS_DSI_MODE M=Mode(TRUE,2,2);CR_MDSS_EXT_IO I=Io(H);
 CR_MDSS_DSC_PARAMS Q={.Revision=1,.MinorVersion=2,.BitsPerComponent=8,.BppQ4=128,
  .PictureWidth=1920,.PictureHeight=1080,.SliceWidth=480,.SliceHeight=8,.BlockPrediction=TRUE};
 M.Dsc=TRUE;M.SlicesPerPacket=1;assert(CrMdssDscBuild(&Q,&M.Compression)==CR_SUCCESS);
 assert(CrMdssDsiInit(D,&I,&M)==CR_SUCCESS);
 CR_MDSS_DSI_COMMAND_MEMORY C={H,H->Command,sizeof(H->Command),0x20000000,CommandReady,Publish};
 assert(CrMdssDsiSetCommandMemory(D,&C)==CR_SUCCESS);
 assert(CrMdssDsiPrepare(D)==CR_SUCCESS);
}
static void Commands(void){HW H;CR_MDSS_DSI D;UINT32 Calls;UINT8 Pps[128];
 Reset(&H);CommandInit(&H,&D);assert(CrMdssDsiEnable(&D)==CR_NOT_READY);
 H.Calls=0;assert(CrMdssDsiSendPps(&D,3)==CR_SUCCESS);Calls=H.Calls;
 assert(H.PacketTriggers==4&&D.PpsSent[0]&&D.PpsSent[1]&&D.CompressionEnabled[1]);
 assert(!memcmp(H.Command,(UINT8[]){128,0,0x0a,0xc0},4));
 assert(CrMdssDscPackPps(&D.Plan.Mode.Compression,3,Pps,128)==CR_SUCCESS);
 assert(!memcmp(H.Command+4,Pps,128));assert(CrMdssDsiEnable(&D)==CR_SUCCESS);
 assert(CrMdssDsiWritePacket(&D,0,5,(UINT8[]){0x28},1,TRUE)==CR_NOT_READY);
 assert(CrMdssDsiDisableVideo(&D)==CR_SUCCESS&&D.Prepared&&!D.Active);
 assert(CrMdssDsiWritePacket(&D,1,5,(UINT8[]){0x28},1,TRUE)==CR_SUCCESS);
 assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 for(UINT32 F=1;F<=Calls;++F)for(UINT32 Warning=0;Warning<2;++Warning){
  Reset(&H);CommandInit(&H,&D);H.Calls=0;H.Fail=F;H.Failure=Warning?1:CR_DEVICE_ERROR;
  assert(CrMdssDsiSendPps(&D,0)!=CR_SUCCESS&&CrMdssDsiEnable(&D)==CR_NOT_READY);
  H.Fail=0;assert(CrMdssDsiUninit(&D)==CR_SUCCESS&&!H.Clock[0]&&!H.Clock[1]);
 }
 Reset(&H);CommandInit(&H,&D);H.NeverComplete=TRUE;
 assert(CrMdssDsiSendPps(&D,0)==CR_TIMEOUT&&D.CleanupPending&&D.CommandTouched[0]);
 H.NeverComplete=FALSE;assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 Reset(&H);CommandInit(&H,&D);H.R[CrMdssExtDsi0][0x110/4]=1U<<24;
 UINT32 Before=H.Writes;
 assert(CrMdssDsiSendPps(&D,0)==CR_DEVICE_ERROR&&!D.CommandTouched[0]&&
  !D.PpsSent[0]&&H.Writes==Before&&H.PacketTriggers==0);
 assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 Reset(&H);CommandInit(&H,&D);H.CommandError=TRUE;
 assert(CrMdssDsiSendPps(&D,0)==CR_DEVICE_ERROR&&D.CleanupPending&&
  D.CommandTouched[0]&&!D.PpsSent[0]&&H.PacketTriggers==1);
 H.CommandError=FALSE;assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 Reset(&H);CommandInit(&H,&D);H.R[CrMdssExtDsi0][0x110/4]=1U<<4;
 assert(CrMdssDsiWritePacket(&D,0,5,(UINT8[]){0x28},1,TRUE)==CR_SUCCESS&&
  (H.R[CrMdssExtDsi0][0x110/4]&(1U<<4)));
 assert(CrMdssDsiUninit(&D)==CR_SUCCESS);
 printf("Actual PPS/DMA/FIFO failure retention: %u phases\n",Calls*2);
}
static CR_STATUS PanelPrepare(VOID *C,CR_MDSS_DSI *D,BOOLEAN *Owned){HW *H=C;
 assert(D->Prepared&&!D->Active);CR_STATUS S=Step(H);
 if(S==CR_SUCCESS||H->CommitFailure){H->PanelOwned=TRUE;*Owned=TRUE;}return S;}
static CR_STATUS PanelStop(VOID *C,CR_MDSS_DSI *D,BOOLEAN *Owned){HW *H=C;(void)D;
 assert(!mDpu[0x3526c/4]&&!mDpu[0x3626c/4]);CR_STATUS S=Step(H);
 if(S==CR_SUCCESS){H->PanelOwned=FALSE;*Owned=FALSE;}return S;}
static CR_STATUS Pixels(VOID *C,VOID *Cpu,UINTN Bytes){assert(Cpu&&Bytes);return Step(C);}
static UINT8 mPixels[1920*1080*4];
static void DisplayInit(HW *H,CR_MDSS_EXT_DISPLAY *D,BOOLEAN Cphy,UINT32 Count,BOOLEAN Dsc){
 CR_MDSS_DSI_MODE M=Mode(Cphy,Count,Cphy?2:4);CR_MDSS_EXT_IO I=Io(H);
 CR_MDSS_EXT_PANEL Panel={H,PanelPrepare,PanelStop,Pixels};
 CR_MDSS_DSI_COMMAND_MEMORY Cmd={H,H->Command,sizeof(H->Command),0x20000000,CommandReady,Publish};
 if(Dsc){CR_MDSS_DSC_PARAMS Q={.Revision=1,.MinorVersion=2,.BitsPerComponent=8,.BppQ4=128,
  .PictureWidth=1920,.PictureHeight=1080,.SliceWidth=1920/Count/2,.SliceHeight=8,.BlockPrediction=TRUE};
  M.Dsc=TRUE;M.SlicesPerPacket=1;assert(CrMdssDscBuild(&Q,&M.Compression)==CR_SUCCESS);}
 UINT32 Before=H->Calls;
 assert(CrMdssExtendedInit(D,&I,&Panel,&M,mPixels,0x10000000,sizeof(mPixels),Dsc?&Cmd:NULL)==CR_SUCCESS);
 assert(H->Calls==Before);
}
static void Compositor(void){HW H;CR_MDSS_EXT_DISPLAY D;UINT32 Start,Stop,Failures=0;
 for(UINT32 C=0;C<2;++C)for(UINT32 N=1;N<=2;++N)for(UINT32 Dsc=0;Dsc<2;++Dsc){
  Reset(&H);DisplayInit(&H,&D,C,N,Dsc);assert(CrMdssExtendedStart(&D)==CR_SUCCESS&&D.Active);
  assert(mDpu[0x24008/4]==0&&mDpu[0x150fc/4]==(1U<<N)-1);
  assert(mDpu[0x15000/4]==((1U<<24)|(1U<<18)));
  if(N==2){assert(mDpu[0x26008/4]==960&&mDpu[0x15004/4]==((1U<<24)|(1U<<21)));
   assert(mDpu[0x2f4/4]==1&&mDpu[0x3f0/4]==(1U<<8)&&mDpu[0x2f8/4]==(1U<<4));
   assert(mDpu[0x45000/4]&(1U<<31));}
  else assert(mDpu[0x2f4/4]==0);
  assert(mDpu[0x35060/4]==(Dsc?1U<<12:1U<<4));
  UINT32 Ref[0x400/4]={0};static CONST UINT32 Off[]={4,8,0xc,0x14,0x1c,0x24,0x2c,0x34,
   0x3c,0x40,0x44,0x48,0x4c,0x50,0x60,0x64,0x68,0x90,0xa8};
  for(UINT32 Link=0;Link<N;++Link){memset(Ref,0,sizeof(Ref));ReferenceDpuTiming(&D.Dsi.Plan,Link,Ref);
   for(UINT32 J=0;J<sizeof(Off)/sizeof(Off[0]);++J)
    assert(mDpu[(0x35000+Link*0x1000+Off[J])/4]==Ref[Off[J]/4]);}
  memset(Ref,0,sizeof(Ref));ReferenceDpuSplit(N==2,Ref);
  assert(mDpu[0x28/4]==Ref[0x28/4]&&mDpu[0x2f4/4]==Ref[0x2f4/4]&&
   mDpu[0x2f8/4]==Ref[0x2f8/4]&&mDpu[0x3f0/4]==Ref[0x3f0/4]);
  if(Dsc){assert(mDpu[0x150e8/4]==(1U<<N)-1&&mDpu[0x690a0/4]==1);
   assert(mDpu[0x80134/4]==((1080U<<16)|1920));assert(H.PacketTriggers==2*N);}
  UINT32 Before=H.Writes;H.Lost[CrMdssExtPhy0+N-1]=TRUE;
  assert(CrMdssExtendedStart(&D)==CR_NOT_READY&&H.Writes==Before);
  H.Lost[CrMdssExtPhy0+N-1]=FALSE;
  H.StopChecking=TRUE;assert(CrMdssExtendedUninit(&D)==CR_SUCCESS&&!H.PanelOwned&&!H.Clock[0]&&!H.Clock[1]);
 }
 for(UINT32 Dsc=0;Dsc<2;++Dsc){
  Reset(&H);DisplayInit(&H,&D,TRUE,2,Dsc);H.Calls=0;assert(CrMdssExtendedStart(&D)==CR_SUCCESS);Start=H.Calls;
  H.Calls=0;H.StopChecking=TRUE;assert(CrMdssExtendedStop(&D)==CR_SUCCESS);Stop=H.Calls;
  for(UINT32 Stage=0;Stage<2;++Stage)for(UINT32 Warning=0;Warning<2;++Warning)
  for(UINT32 F=1;F<=(Stage?Stop:Start);++F){
   Reset(&H);DisplayInit(&H,&D,TRUE,2,Dsc);if(Stage)assert(CrMdssExtendedStart(&D)==CR_SUCCESS);
   H.Calls=0;H.Fail=F;H.Failure=Warning?1:CR_DEVICE_ERROR;H.CommitFailure=TRUE;
   assert((Stage?CrMdssExtendedStop(&D):CrMdssExtendedStart(&D))!=CR_SUCCESS&&D.CleanupPending);
   assert(CrMdssExtendedStart(&D)==CR_NOT_READY);H.Fail=0;H.StopChecking=TRUE;
   assert(CrMdssExtendedUninit(&D)==CR_SUCCESS&&!H.PanelOwned&&!H.Clock[0]&&!H.Clock[1]);++Failures;
  }
 }
 Reset(&H);DisplayInit(&H,&D,TRUE,2,TRUE);assert(CrMdssExtendedStart(&D)==CR_SUCCESS);
 H.StuckTiming=TRUE;assert(CrMdssExtendedStop(&D)==CR_TIMEOUT&&H.PanelOwned&&H.Clock[0]&&H.Clock[1]);
 H.StuckTiming=FALSE;assert(CrMdssExtendedUninit(&D)==CR_SUCCESS);
 Reset(&H);DisplayInit(&H,&D,FALSE,2,FALSE);assert(CrMdssExtendedStart(&D)==CR_SUCCESS);
 H.StuckReset=TRUE;assert(CrMdssExtendedStop(&D)==CR_TIMEOUT&&H.PanelOwned&&H.Clock[0]);
 H.StuckReset=FALSE;assert(CrMdssExtendedUninit(&D)==CR_SUCCESS);
 printf("Actual dual DPU/DSC compositor fault retention: %u phases\n",Failures);
}
static void SocProfiles(void){
 HW H;CR_MDSS_DSI D;CR_MDSS_DPU_EXT Dpu;CR_MDSS_DSI_PLAN P,Old;
 CONST CR_MDSS_HW_PROFILE *Cmd=CrMdssGetHardwareProfile(0x8250);
 assert(Cmd&&Cmd->Capabilities==CR_MDSS_HW_COMMAND&&Cmd->DpuMajor==6&&Cmd->PhyRevision==0x410);
 assert(Cmd->Ctl==0x1000&&Cmd->Pingpong[0]==0x70000&&Cmd->Intf[0]==0x6a800&&Cmd->IntfBytes==0x2c0);
 for(UINT32 C=0;C<2;++C)for(UINT32 Fast=0;Fast<2;++Fast)for(UINT32 Count=1;Count<=2;++Count){
  Reset(&H);CR_MDSS_EXT_IO I=Io(&H);CR_MDSS_DSI_MODE M=Mode(C,Count,C?2:4);
  M.Raster.Soc=0x8350;M.PixelHz*=Fast?2:1;M.Raster.RefreshQ16*=Fast?2:1;
  assert(CrMdssDsiInit(&D,&I,&M)==CR_SUCCESS);
  assert(CrMdssValidateMode(&M.Raster,(UINTN)-1)==CR_INVALID_PARAMETER);
  assert(CrMdssDsiPrepare(&D)==CR_SUCCESS&&H.RefgenWrites==0);
  /* Public PHY4.2 DPHY low/high RESCODE is 0x3c/0; CPHY uses 0x3d/1. */
  assert(H.R[CrMdssExtPhy0][0xf4/4]==(C?(D.Plan.SymbolHz<=1500000000ULL?0x3dU:1U):
         (D.Plan.SymbolHz<=1500000000ULL?0x3cU:0U)));
  assert(H.R[CrMdssExtPhy0][0xf8/4]==(D.Plan.SymbolHz<=1500000000ULL?0x38U:C?0x3bU:0x39U));
  assert(CrMdssDpuExtendedInit(&Dpu,&D,0x24000000,1920*1080*4)==CR_SUCCESS);
  assert(CrMdssDpuExtendedPrepare(&Dpu)==CR_SUCCESS);
  assert(mDpu[0x150f4/4]==(Count==1?2U:6U)&&mDpu[0x150fc/4]==(1U<<Count)-1);
  assert(mDpu[0x35060/4]==(1U<<4)&&mDpu[0x35090/4]==0x213f);
  assert(CrMdssDpuExtendedStop(&Dpu)==CR_SUCCESS);
  assert(CrMdssDsiStop(&D)==CR_SUCCESS&&H.RefgenWrites==0);
  assert(CrMdssDpuExtendedUninit(&Dpu)==CR_SUCCESS&&CrMdssDsiUninit(&D)==CR_SUCCESS);
 }
 /* A command-only catalog may not enter this continuous-video core. */
 CR_MDSS_DSI_MODE M=Mode(FALSE,1,4);M.Raster.Soc=0x8250;
 memset(&P,0xa5,sizeof(P));Old=P;
 assert(CrMdssDsiBuildPlan(&M,&P)==CR_UNSUPPORTED&&!memcmp(&P,&Old,sizeof(P)));
 M.Raster.Soc=0x9999;
 assert(CrMdssDsiBuildPlan(&M,&P)==CR_UNSUPPORTED&&!memcmp(&P,&Old,sizeof(P)));
 M.Raster.Soc=0x8350;M.Dsc=TRUE;
 assert(CrMdssDsiBuildPlan(&M,&P)==CR_UNSUPPORTED&&!memcmp(&P,&Old,sizeof(P)));
 puts("SM8350 shared video/PHY profile and command-only exclusion PASS");
}
int main(void){SocProfiles();Pure();Happy();Faults();Commands();Compositor();puts("extended DSI source/lifetime PASS");return 0;}
