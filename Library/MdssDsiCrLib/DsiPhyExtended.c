/* SM8450 PHY4.3 5nm DPHY/CPHY and master/slave bonded PLL.
 * Copyright (c) 2015-2021 The Linux Foundation.
 * Android dsi_phy_hw_v4_0.c/dsi_pll_5nm.c; mainline dsi_phy_7nm.c.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdssDsiInternal.h"
#define ARRAY_COUNT(x) (sizeof(x)/sizeof((x)[0]))
typedef struct { UINT16 Offset; UINT8 Value; } PLL_VALUE;
STATIC CONST PLL_VALUE mPllFixed[] = {
  { 0x018, 0x01 }, { 0x004, 0x03 }, { 0x010, 0x00 }, { 0x020, 0x00 },
  { 0x024, 0x4E }, { 0x044, 0x40 }, { 0x068, 0xBA }, { 0x078, 0x0C },
  { 0x0A8, 0x00 }, { 0x0B8, 0x00 }, { 0x0C8, 0x08 }, { 0x160, 0x0A },
  { 0x168, 0xC0 }, { 0x170, 0x84 }, { 0x170, 0x82 }, { 0x178, 0x4C },
  { 0x190, 0x80 }, { 0x090, 0x29 }, { 0x090, 0x2F }, { 0x094, 0x2A },
  { 0x094, 0x3F }, { 0x260, 0x22 },
};


CR_STATUS CrMdssDsiPhyPrepare(CR_MDSS_DSI *D)
{
 CONST CR_MDSS_DSI_PLAN *P;UINT32 I,J,C,V,Out=0,Inverter,Lane0=0;
 UINT64 Divider;CR_STATUS S;BOOLEAN Cphy;
 STATIC CONST UINT8 Drive[5]={0x40,0x40,0x40,0x46,0x41};
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 P=&D->Plan;Cphy=P->Mode.PhyType==CrMdssCphy;
 /* Verify every member before changing the first; another live PLL or gated
    global clock is evidence that the retired-owner fence has not completed. */
 for(I=0;I<P->Mode.Controllers;++I){
  CR_MDSS_EXT_WINDOW W=XPhy(P->Link[I].Controller);
  S=XRead(D,W,0x3c,&V);if(S!=CR_SUCCESS)return S;if(V&1)return CR_BUSY;
  S=XRead(D,W,0x14,&V);if(S!=CR_SUCCESS)return S;if(V&XB(5))return CR_BUSY;
 }
 for(J=0;J<4;++J)if(P->Mode.LaneMap[0]==(1U<<J))Lane0=J;
 while((1U<<Out)<P->PllPostDivider)++Out;
 for(I=0;I<P->Mode.Controllers;++I){
  C=P->Link[I].Controller;CR_MDSS_EXT_WINDOW W=XPhy(C);D->PhyTouched[I]=TRUE;
#define PPUT(o,v) do{S=XWrite(D,W,(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
  PPUT(0x1ac,1);S=XDelay(D,W,500);if(S!=CR_SUCCESS)return S;
  S=XPoll(D,W,0x140,1,1,1000);if(S!=CR_SUCCESS)return S;
  PPUT(0x24,0x60);PPUT(0x3c,0);PPUT(0x1c,0);
  S=XRead(D,W,0,&V);if(S!=CR_SUCCESS)return S;if((V&0xf0)>=0x20)PPUT(0x114,4);
  PPUT(0x34,P->Mode.LaneMap[0]|(P->Mode.LaneMap[1]<<4));
  PPUT(0x38,P->Mode.LaneMap[2]|(P->Mode.LaneMap[3]<<4));
  PPUT(0x18,Cphy?XB(6):0);
  PPUT(0x20,Cphy?0x51:P->SymbolHz<=1500000000ULL?0x53:0x52);
  PPUT(0x110,Cphy?0x55:0x5c);PPUT(0x30,0);PPUT(0x10c,0);
  PPUT(0xec,Cphy?0:0x88);PPUT(0x104,Cphy?0x11:0);PPUT(0x108,Cphy?1:0);
  PPUT(0xf4,P->SymbolHz<=1500000000ULL?0x3d:1);
  PPUT(0xf8,P->SymbolHz<=1500000000ULL?0x38:Cphy?0x3b:0x39);
  PPUT(0x100,0x55);PPUT(0x24,0x7f);PPUT(0xa0,Cphy?0x17:0x1f);
  PPUT(0x2c,Cphy?0:0x40);PPUT(0x14,P->PixelMux|(I?XB(2):0));
  for(J=0;J<14;++J)PPUT(0xb4+J*4,P->Timings[J]);
  for(J=0;J<5;++J){UINT32 L=0x200+J*0x80;
   PPUT(L+0x14,J==Lane0?3:0);PPUT(L+0x10,0);PPUT(L,0);PPUT(L+4,0);
   PPUT(L+8,J==4?0x8a:0x0a);PPUT(L+0x18,Drive[J]);
  }
  /* Slave receives the master's output and uses postdivider1. */
  PPUT(0x10,(P->PixelDivider<<4)|(I?1:P->PhyPostDivider));
  PPUT(0x654,Out);PPUT(0x760,0x22);PPUT(0x528,0xc0);
  S=XDelay(D,W,1);if(S!=CR_SUCCESS)return S;
#undef PPUT
 }
 /* Only the first link's PLL is programmed/started. */
 CR_MDSS_EXT_WINDOW W=XPhy(P->Link[0].Controller);
 Divider=(P->VcoHz<<18)/38400000ULL;
 Inverter=P->VcoHz<=1000000000ULL?0xa0:P->VcoHz<=2500000000ULL?0x20:
   P->VcoHz<=3500000000ULL?0:0x40;
#define MPLL(o,v) do{S=XWrite(D,W,0x500+(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
 MPLL(0xbc,0x12);MPLL(0xe0,(UINT32)(Divider>>18));MPLL(0xe4,(UINT32)Divider&255);
 MPLL(0xe8,((UINT32)Divider>>8)&255);MPLL(0xec,((UINT32)Divider>>16)&3);
 MPLL(0x158,0x40);MPLL(0x194,6);MPLL(0x250,Cphy?0:0x10);MPLL(0x248,Inverter);
 MPLL(0x258,P->VcoHz>=3100000000ULL?3:1);
 MPLL(0x240,P->VcoHz<1520000000ULL?8:P->VcoHz>=2990000000ULL?1:0);
 for(J=0;J<ARRAY_COUNT(mPllFixed);++J)MPLL(mPllFixed[J].Offset,mPllFixed[J].Value);
 MPLL(0x150,0);
#undef MPLL
 S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 S=XWrite(D,W,0x3c,1);if(S!=CR_SUCCESS)return S;
 S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 S=XPoll(D,W,0x6b0,1,1,5000);if(S!=CR_SUCCESS)return S;
 for(I=0;I<P->Mode.Controllers;++I){W=XPhy(P->Link[I].Controller);
  S=XChecked(D,W,0x128,1,1);if(S!=CR_SUCCESS)return S;
  S=XChecked(D,W,0x128,0,1);if(S!=CR_SUCCESS)return S;
  S=XWrite(D,W,0x30,4);if(S!=CR_SUCCESS)return S;
  S=XUpdate(D,W,0x14,XB(5)|XB(4),XB(5)|XB(4));if(S!=CR_SUCCESS)return S;
  S=XChecked(D,W,0x1c,1,0xff);if(S!=CR_SUCCESS)return S;
  if(!Cphy&&P->Mode.ContinuousClock){S=XUpdate(D,W,0xa4,XB(5)|XB(6),XB(5)|XB(6));if(S!=CR_SUCCESS)return S;}
 }
 return CR_SUCCESS;
}
CR_STATUS CrMdssDsiPhyStop(CR_MDSS_DSI *D)
{
 UINT32 I,J;CR_STATUS S;
 STATIC CONST struct {UINT32 Offset,Mask,Delay;} Steps[]={
  {0xa4,XB(5)|XB(6),0},{0x14,XB(5),0},{0x3c,1,0},{0x1c,255,0},
  {0x528,255,0},{0x24,XB(5),1},{0x1ac,255,2},{0x24,0x1f,0},
  {0xa0,255,0},{0x24,255,0}};
 if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
 /* Slave is fully gated before master PLL shutdown. No old clock source can
    feed a controller after any lower dependency has been withdrawn. */
 for(I=D->Plan.Mode.Controllers;I>0;--I)if(D->PhyTouched[I-1]){
  CR_MDSS_EXT_WINDOW W=XPhy(D->Plan.Link[I-1].Controller);
  for(J=0;J<ARRAY_COUNT(Steps);++J){
   S=XUpdate(D,W,Steps[J].Offset,Steps[J].Mask,0);if(S!=CR_SUCCESS)return S;
   if(Steps[J].Delay){S=XDelay(D,W,Steps[J].Delay);if(S!=CR_SUCCESS)return S;}
  }
  for(J=0;J<5;++J){S=XChecked(D,W,0x214+J*0x80,0,255);if(S!=CR_SUCCESS)return S;}
  D->PhyTouched[I-1]=FALSE;
 }
 return CR_SUCCESS;
}
