/* Copyright (c) 2016-2021 The Linux Foundation.
 * Adapted from msm_dsi_cphy_timing_calc_v4/dsi_host and Qualcomm 5nm PLL.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssDsiCrLib.h>
#include <oskal/cr_string.h>
#define B(n) (1U<<(n))
#define UP(v,d) (((v)+(d)-1)/(d))
STATIC INT32
RoundUp (INT32 Value, INT32 Divisor)
{
  return Value >= 0 ? (Value + Divisor - 1) / Divisor :
                      (Value - Divisor + 1) / Divisor;
}

STATIC UINT32
Interpolate (INT32 Maximum, INT32 Minimum, INT32 Percent)
{
  INT32 Result = RoundUp ((Maximum - Minimum) * Percent, 100) + Minimum;
  return (UINT32)(Result > 0 ? Result : 0);
}

/* Linux msm_dsi_dphy_timing_calc_v4, with 1000x nanosecond precision. */
STATIC CR_STATUS
DphyTimings (UINT64 BitRate, UINT32 *Values)
{
  INT32 Ui, Ui8, Min, Max;
  UINT32 Index;
  if ((BitRate < 80000000ULL) || (BitRate > 2500000000ULL)) {
    return CR_UNSUPPORTED;
  }
  Ui = (INT32)(1000000000ULL / (BitRate / 1000));
  Ui8 = Ui * 8;
  for (Index = 0; Index < 14; ++Index) Values[Index] = 0;
  Values[2] = Interpolate (95000 / Ui8, RoundUp (38000, Ui8), 50);
  Min = RoundUp (300000 - (INT32)Values[2] * Ui8, Ui8) - 1;
  Max = Min > 255 ? 511 : 255;
  Values[1] = Interpolate (Max, Min, 2);
  Values[3] = Interpolate ((85000 + 15 * Ui) / Ui8,
                          RoundUp (60000 + 3 * Ui, Ui8), 30);
  Values[6] = Interpolate ((85000 + 6 * Ui) / Ui8,
                          RoundUp (40000 + 4 * Ui, Ui8), 50);
  Min = RoundUp (145000 + 10 * Ui - (INT32)Values[6] * Ui8, Ui8) - 1;
  Values[5] = Interpolate (255, Min, 10);
  Values[7] = Interpolate ((85000 + 12 * Ui) / Ui8 - 1,
                          RoundUp (60000 + 4 * Ui, Ui8) - 1, 30);
  Values[8] = (UINT32)RoundUp (50000 - 8 * Ui, Ui8);
  Values[4] = Interpolate (255, RoundUp (100000, Ui8) - 1, 10);
  Min = RoundUp (60000 + 52 * Ui + ((INT32)Values[7] + 1) * Ui8, 16 * Ui) - 1;
  Values[13] = Interpolate (255, Min, 5);
  Min = RoundUp (106000 + ((INT32)Values[2] + (INT32)Values[1] + 1) * Ui8,
                 16 * Ui) - 1;
  Values[12] = (UINT32)(RoundUp ((255 - Min) * 125, 10000) + Min);
  Values[9] = 2;
  Values[10] = 4;
  for (Index = 0; Index < 14; Index++) {
    if (Values[Index] > 255) {
      return CR_UNSUPPORTED;
    }
  }
  return CR_SUCCESS;
}


STATIC CR_STATUS CphyTimings(UINT64 Symbol, UINT32 *T)
{
  INT32 Ui,Ui7,Min;UINT32 I;
  if(Symbol<80000000ULL||Symbol>2500000000ULL)return CR_UNSUPPORTED;
  Ui=(INT32)(1000000000ULL/(Symbol/1000));Ui7=Ui*7;
  for(I=0;I<14;++I)T[I]=0;
  T[6]=Interpolate(95000/Ui7,RoundUp(38000,Ui7),50);
  T[8]=Interpolate(255,RoundUp(50000,Ui7),1);
  T[4]=Interpolate(255,RoundUp(100000,Ui7)-1,10);
  T[7]=Interpolate(32,1,80);
  Min=RoundUp(262000,Ui7)-1;if(Min>64)Min=64;
  T[5]=Interpolate(64,Min,20);T[9]=2;T[10]=4;
  for(I=0;I<14;++I)if(T[I]>255)return CR_UNSUPPORTED;
  return CR_SUCCESS;
}
typedef struct {UINT64 Min,Max;UINT32 Pll,Phy;} DIVIDER;
STATIC CONST DIVIDER mDphy[]={
 {27270000,30000000,2,11},{30000000,33330000,4,5},{33330000,37500000,2,9},
 {37500000,40000000,8,2},{40000000,42860000,1,15},{42860000,46150000,2,7},
 {46150000,50000000,1,13},{50000000,54550000,4,3},{54550000,60000000,1,11},
 {60000000,66670000,2,5},{66670000,75000000,1,9},{75000000,85710000,8,1},
 {85710000,100000000,1,7},{100000000,120000000,2,3},{120000000,150000000,1,5},
 {150000000,200000000,4,1},{200000000,300000000,1,3},{300000000,600000000,2,1},
 {600000000,5000000000ULL,1,1}};
STATIC CONST DIVIDER mCphy[]={
 {30000000,37500000,4,5},{37500000,50000000,8,2},{50000000,60000000,4,3},
 {60000000,75000000,2,5},{75000000,100000000,8,1},{100000000,120000000,2,3},
 {120000000,150000000,1,5},{150000000,200000000,4,1},{200000000,300000000,1,3},
 {300000000,600000000,2,1},{600000000,5000000000ULL,1,1}};
CR_STATUS CrMdssDsiBuildPlan(CONST CR_MDSS_DSI_MODE *M,CR_MDSS_DSI_PLAN *Out)
{
  CR_MDSS_DSI_PLAN P;CR_STATUS S;UINT64 H,V,Refresh,NewH,Width,PerLinkBytes;UINT32 I,Seen=0;
  CONST DIVIDER *Table;UINTN Count;
  if(!M||!Out||M->Revision!=1||M->Controllers<1||M->Controllers>2||
      M->FirstController>1||(M->Controllers==2&&M->FirstController!=0)||
      M->PhyType<CrMdssDphy||M->PhyType>CrMdssCphy||M->ContinuousClock>1||M->Dsc>1||
      !M->PixelHz||M->PixelHz>1000000000ULL)return CR_INVALID_PARAMETER;
  S=CrMdssValidateMode(&M->Raster,(UINTN)-1);if(S!=CR_SUCCESS)return S;
  if(M->Raster.Controller!=M->FirstController||
      (M->PhyType==CrMdssCphy&&M->Raster.Lanes>3))return CR_INVALID_PARAMETER;
  if(M->Raster.InvertHsync||M->Raster.InvertVsync)return CR_UNSUPPORTED;
  for(I=0;I<4;++I){UINT32 Lane=M->LaneMap[I];
    if(!Lane||(Lane&(Lane-1))||(Lane&~15)||(Seen&Lane))return CR_INVALID_PARAMETER;
    Seen|=Lane;
  }
  if(M->Controllers==2&&((M->Raster.Width|M->Raster.HSync|
      M->Raster.HBackPorch|M->Raster.HFrontPorch)&1))return CR_UNSUPPORTED;
  cr_memset(&P,0,sizeof(P));P.Mode=*M;
  H=(UINT64)M->Raster.Width+M->Raster.HSync+M->Raster.HBackPorch+M->Raster.HFrontPorch;
  V=(UINT64)M->Raster.Height+M->Raster.VSync+M->Raster.VBackPorch+M->Raster.VFrontPorch;
  Refresh=M->PixelHz*65536/(H*V);
  if((Refresh>M->Raster.RefreshQ16?Refresh-M->Raster.RefreshQ16:M->Raster.RefreshQ16-Refresh)>1)
    return CR_INVALID_PARAMETER;
  Width=M->Raster.Width/M->Controllers;
  P.PayloadWidth=(UINT32)Width;
  if(M->Dsc){
    S=CrMdssDscValidate(&M->Compression);if(S!=CR_SUCCESS)return S;
    if(M->Compression.Params.BitsPerComponent!=8)return CR_UNSUPPORTED;
    if(M->Compression.Params.PictureWidth!=M->Raster.Width||
       M->Compression.Params.PictureHeight!=M->Raster.Height||
       Width%M->Compression.Params.SliceWidth||!M->SlicesPerPacket)
      return CR_INVALID_PARAMETER;
    I=(UINT32)Width/M->Compression.Params.SliceWidth;
    if(I%M->SlicesPerPacket)return CR_UNSUPPORTED;
    P.PacketsPerLine=I/M->SlicesPerPacket;
    if(P.PacketsPerLine!=1&&P.PacketsPerLine!=2&&P.PacketsPerLine!=4)
      return CR_UNSUPPORTED;
    PerLinkBytes=(UINT64)M->Compression.ChunkBytes*I;
    P.PacketBytes=M->Compression.ChunkBytes*M->SlicesPerPacket;
    if(P.PacketBytes>65535)return CR_BAD_BUFFER_SIZE;
    P.EndOfLineBytes=(UINT32)(PerLinkBytes%3);
    P.PayloadWidth=(UINT32)UP(PerLinkBytes,3);
  }else if(M->SlicesPerPacket)return CR_INVALID_PARAMETER;
  NewH=H-M->Raster.Width+(UINT64)P.PayloadWidth*M->Controllers;
  P.PixelHz=M->PixelHz*NewH/H/M->Controllers;
  P.ByteInterfaceDivider=M->PhyType==CrMdssCphy?1:2;
  if(M->PhyType==CrMdssCphy)P.ByteHz=P.PixelHz*24/(16U*M->Raster.Lanes);
  else{
    /* Android rounds the lane bit rate to the nearest byte, then makes the
       byte clock even so the byte-interface /2 request remains integral. */
    P.ByteHz=(P.PixelHz*24/M->Raster.Lanes+4)/8;
    P.ByteHz=(P.ByteHz+1)&~1ULL;
  }
  P.SymbolHz=P.ByteHz*(M->PhyType==CrMdssCphy?7U:8U);
  if(M->PhyType==CrMdssCphy){Table=mCphy;Count=sizeof(mCphy)/sizeof(mCphy[0]);
    S=CphyTimings(P.SymbolHz,P.Timings);
  }else{Table=mDphy;Count=sizeof(mDphy)/sizeof(mDphy[0]);S=DphyTimings(P.SymbolHz,P.Timings);}
  if(S!=CR_SUCCESS)return S;
  for(I=0;I<Count;++I)if(P.SymbolHz>=Table[I].Min&&P.SymbolHz<=Table[I].Max)break;
  if(I==Count)return CR_UNSUPPORTED;
  P.PllPostDivider=Table[I].Pll;P.PhyPostDivider=Table[I].Phy;
  P.VcoHz=P.SymbolHz*P.PllPostDivider*P.PhyPostDivider;
  if(P.VcoHz<600000000ULL||P.VcoHz>5000000000ULL)return CR_UNSUPPORTED;
  if(M->PhyType==CrMdssCphy){
    P.PixelM=M->Raster.Lanes==2?2:1;P.PixelN=M->Raster.Lanes==2?3:1;
    P.PixelMux=3;P.PixelDivider=24*P.PhyPostDivider*P.PixelM/P.PixelN/8/M->Raster.Lanes;
    P.HostClockTiming=(P.Timings[5]&0x3f)|((P.Timings[7]&0x3f)<<8);
  }else{
    P.PixelM=P.PixelN=1;P.PixelMux=1;
    P.PixelDivider=24/2/M->Raster.Lanes;
    P.HostClockTiming=(P.Timings[12]&0x3f)|((P.Timings[13]&0x3f)<<8);
  }
  if(!P.PixelDivider||P.PixelDivider>15)return CR_UNSUPPORTED;
  P.PixelSourceHz=M->PhyType==CrMdssCphy?
    P.VcoHz/P.PllPostDivider*2/7/P.PixelDivider:
    P.VcoHz/P.PllPostDivider/P.PhyPostDivider/2/P.PixelDivider;
  for(I=0;I<M->Controllers;++I){
    P.Link[I]=M->Raster;P.Link[I].Controller=M->FirstController+I;
    P.Link[I].Width=(UINT32)Width;P.Link[I].HSync/=M->Controllers;
    P.Link[I].HBackPorch/=M->Controllers;P.Link[I].HFrontPorch/=M->Controllers;
  }
  *Out=P;return CR_SUCCESS;
}

#include "MdssDsiInternal.h"
CR_STATUS CrMdssDsiInit(CR_MDSS_DSI *D,CONST CR_MDSS_EXT_IO *Io,CONST CR_MDSS_DSI_MODE *M)
{
 CR_MDSS_DSI_PLAN P;CR_STATUS S;UINT32 I,C;
 if(!D||!Io||!Io->Ready||!Io->Read32||!Io->Write32||!Io->Fence||!Io->DelayUs||
    !Io->ExternalClocks)return CR_INVALID_PARAMETER;
 S=CrMdssDsiBuildPlan(M,&P);if(S!=CR_SUCCESS)return S;
 for(I=0;I<M->Controllers;++I){C=M->FirstController+I;
  if(Io->Physical[XHost(C)]!=(C?0xae96000ULL:0xae94000ULL)||
     Io->Physical[XPhy(C)]!=(C?0xae96400ULL:0xae94400ULL))return CR_UNSUPPORTED;
  if(Io->Bytes[XHost(C)]<0x400||Io->Bytes[XPhy(C)]<0x800||
     Io->Bytes[XHost(C)]>(UINT64)-1-Io->Physical[XHost(C)]||
     Io->Bytes[XPhy(C)]>(UINT64)-1-Io->Physical[XPhy(C)])return CR_BAD_BUFFER_SIZE;
 }
 cr_memset(D,0,sizeof(*D));D->Io=*Io;D->Plan=P;D->Initialized=TRUE;return CR_SUCCESS;
}
STATIC CR_STATUS HostPrepare(CR_MDSS_DSI *D,UINT32 I)
{
 CONST CR_MDSS_MODE *M=&D->Plan.Link[I];CR_MDSS_EXT_WINDOW W=XHost(M->Controller);
 UINT32 Ha=M->HSync+M->HBackPorch,Va=M->VSync+M->VBackPorch;
 UINT32 H=Ha+D->Plan.PayloadWidth+M->HFrontPorch;
 UINT32 V=Va+M->Height+M->VFrontPorch;UINT32 Value;CR_STATUS S;
 S=XRead(D,W,4,&Value);if(S!=CR_SUCCESS)return S;
 if(Value&7)return CR_BUSY; /* callback-retired native owner must first fence */
 D->HostTouched[I]=TRUE;
#define HPUT(o,v) do{S=XWrite(D,W,(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
 HPUT(0x11c,0x23f);HPUT(0x118,1);S=XDelay(D,W,20000);if(S!=CR_SUCCESS)return S;
 HPUT(0x118,0);S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 HPUT(0x24,Ha|((Ha+D->Plan.PayloadWidth)<<16));HPUT(0x28,Va|((Va+M->Height)<<16));
 HPUT(0x2c,(H-1)|((V-1)<<16));HPUT(0x30,M->HSync<<16);HPUT(0x34,0);HPUT(0x38,M->VSync<<16);
 HPUT(0x10,M->VirtualChannel|(3U<<4)|(M->TrafficMode<<8)|
   (M->HsaHseAfterVsVe?XB(28):0)|M->VideoFlags);
 HPUT(0x20,0);HPUT(0xc4,D->Plan.HostClockTiming);HPUT(0x180,0);
 /* DSI v2.x encodes the one-hot lane_map_v2 entries as four nibbles. */
 HPUT(0xb0,D->Plan.Mode.LaneMap[0]|(D->Plan.Mode.LaneMap[1]<<4)|
   (D->Plan.Mode.LaneMap[2]<<8)|(D->Plan.Mode.LaneMap[3]<<12));
 /* Android DSI6G video timing commit, after all active/total fields. */
 HPUT(0xbc,0x3fd08);HPUT(0xa4,0x10100);HPUT(0x1e4,1);
 HPUT(0xcc,M->DisableEot?0:1);HPUT(0x2d8,D->Plan.Mode.PhyType==CrMdssCphy?1:0);
 Value=D->Plan.Mode.Dsc?(D->Plan.PacketBytes<<16)|(0x0b<<8)|
   ((D->Plan.PacketsPerLine>>1)<<6)|(D->Plan.EndOfLineBytes<<4)|1:0;
 HPUT(0x2a0,Value);HPUT(0x2a4,0);
 /* No split-link or inherited command broadcast in a bonded video path. */
 HPUT(0x330,0);HPUT(0x84,0);
 S=XUpdate(D,W,0xac,XB(24)|XB(28),
   D->Plan.Mode.ContinuousClock&&D->Plan.Mode.PhyType==CrMdssDphy?XB(28):0);
 if(S!=CR_SUCCESS)return S;
 Value=XB(8)|1|(((1U<<M->Lanes)-1)<<4);
 S=XChecked(D,W,4,Value,0x1ff);if(S!=CR_SUCCESS)return S;
#undef HPUT
 return CR_SUCCESS;
}
STATIC CR_STATUS MembersReady(CR_MDSS_DSI *D)
{
 UINT32 I;CR_STATUS S;
 for(I=0;I<D->Plan.Mode.Controllers;++I){
  S=XReady(D,XPhy(D->Plan.Link[I].Controller));if(S!=CR_SUCCESS)return S;
  S=XReady(D,XHost(D->Plan.Link[I].Controller));if(S!=CR_SUCCESS)return S;
 }
 return CR_SUCCESS;
}
CR_STATUS CrMdssDsiPrepare(CR_MDSS_DSI *D)
{
 UINT32 I;BOOLEAN Owned;CR_STATUS S;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(D->Prepared&&!D->CleanupPending)return MembersReady(D);
 if(D->CleanupPending)return CR_NOT_READY;
 D->CleanupPending=TRUE;S=CrMdssDsiPhyPrepare(D);if(S!=CR_SUCCESS)return S;
 for(I=0;I<D->Plan.Mode.Controllers;++I){
  Owned=FALSE;S=XExact(D->Io.ExternalClocks(D->Io.Context,D->Plan.Link[I].Controller,&D->Plan,TRUE,&Owned));
  D->ExternalTouched[I]=Owned;if(S!=CR_SUCCESS)return S;if(!Owned)return CR_NOT_READY;
  S=HostPrepare(D,I);if(S!=CR_SUCCESS)return S;
 }
 D->Prepared=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiEnable(CR_MDSS_DSI *D)
{
 UINT32 I;CR_STATUS S;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(!D->Prepared||D->CleanupPending)return CR_NOT_READY;
 if(D->Plan.Mode.Dsc)for(I=0;I<D->Plan.Mode.Controllers;++I)
  if(!D->PpsSent[I]||!D->CompressionEnabled[I])return CR_NOT_READY;
 if(D->Active)return MembersReady(D);
 D->CleanupPending=TRUE;
 /* Slave host first, then the master; DPU split synchronization controls the
    timing release. Never run one prepared host after the other's failure. */
 for(I=D->Plan.Mode.Controllers;I>0;--I){
  S=XUpdate(D,XHost(D->Plan.Link[I-1].Controller),4,XB(1),XB(1));if(S!=CR_SUCCESS)return S;
 }
 D->Active=TRUE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiDisableVideo(CR_MDSS_DSI *D)
{
 UINT32 I;CR_STATUS S;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 if(!D->Prepared||D->CleanupPending)return CR_NOT_READY;
 D->CleanupPending=TRUE;
 for(I=D->Plan.Mode.Controllers;I>0;--I){
  S=XUpdate(D,XHost(D->Plan.Link[I-1].Controller),4,7,0);if(S!=CR_SUCCESS)return S;
  /* With the DPU timing engine stopped, video no longer reaches a boundary.
     Linux resets the video engine before sending panel-off commands. */
  S=HostPrepare(D,I-1);if(S!=CR_SUCCESS)return S;
 }
 D->Active=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiStop(CR_MDSS_DSI *D)
{
 UINT32 I;CR_STATUS S;BOOLEAN Owned;
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 D->CleanupPending=TRUE;
 for(I=D->Plan.Mode.Controllers;I>0;--I)if(D->HostTouched[I-1]){
  CR_MDSS_EXT_WINDOW W=XHost(D->Plan.Link[I-1].Controller);
  S=XUpdate(D,W,4,7,0);if(S!=CR_SUCCESS)return S;
  S=XWrite(D,W,0x118,1);if(S!=CR_SUCCESS)return S;
  S=XDelay(D,W,20000);if(S!=CR_SUCCESS)return S;
  S=XWrite(D,W,0x118,0);if(S!=CR_SUCCESS)return S;
  S=XFence(D,W);if(S!=CR_SUCCESS)return S;S=XPoll(D,W,4,7,0,1000);if(S!=CR_SUCCESS)return S;
  D->HostTouched[I-1]=FALSE;
  D->CommandTouched[I-1]=D->PpsSent[I-1]=D->CompressionEnabled[I-1]=FALSE;
 }
 /* All hosts are inert before byte/pixel gates or shared master PLL retire. */
 for(I=D->Plan.Mode.Controllers;I>0;--I)if(D->ExternalTouched[I-1]){
  Owned=TRUE;S=XExact(D->Io.ExternalClocks(D->Io.Context,D->Plan.Link[I-1].Controller,&D->Plan,FALSE,&Owned));
  if(S!=CR_SUCCESS)return S;if(Owned)return CR_NOT_READY;D->ExternalTouched[I-1]=FALSE;
 }
 S=CrMdssDsiPhyStop(D);if(S!=CR_SUCCESS)return S;
 D->Prepared=FALSE;D->Active=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiUninit(CR_MDSS_DSI *D)
{
 CR_STATUS S;if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 S=CrMdssDsiStop(D);if(S!=CR_SUCCESS)return S;cr_memset(D,0,sizeof(*D));return CR_SUCCESS;
}
