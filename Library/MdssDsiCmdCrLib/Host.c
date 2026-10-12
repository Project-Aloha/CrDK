/* Qualcomm DSI 2.4 command transport, cad09e0 dsi_ctrl_hw_cmn.c.
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
#define HOST CrMdssCmdHost
#define TRY(x) do { S=(x); if(S!=CR_SUCCESS)return S; } while(0)
#define PUT(o,v) TRY(CmdWrite(D,HOST,(o),(v)))
#define CHECK(o,v,m) TRY(CmdChecked(D,HOST,(o),(v),(m)))
#define FORCE_CLOCKS (0x23fU|CMDBIT(8)|CMDBIT(9)|CMDBIT(11)|CMDBIT(21))

CR_STATUS CmdHostStart(CR_MDSS_CMD *D)
{
  CR_STATUS S; UINT32 V;
  TRY(CmdPlanCheck(D));
  if(D->HostTouched)return CR_NOT_READY;
  TRY(CmdRead(D,HOST,0x4,&V));
  /* A retained boot display must first be quiesced by its actual owner.
   * Do not reset an unrelated running engine just to acquire this route. */
  if(V&7)return CR_NOT_READY;
  TRY(CmdPoll(D,HOST,0x8,CMDBIT(1)|CMDBIT(2),0,2000));
  D->HostTouched=TRUE;
  PUT(0x11c,FORCE_CLOCKS); TRY(CmdFence(D,HOST));
  PUT(0x118,1); TRY(CmdFence(D,HOST)); TRY(CmdDelay(D,HOST,1));
  PUT(0x118,0); TRY(CmdFence(D,HOST));
  /* External TE, software DMA trigger, MDP trigger NONE; VC/stream zero. */
  CHECK(0x84,CMDBIT(31)|4,CMDBIT(31)|0x77);CHECK(0xc4,D->Plan.HostClockTiming,0x3f3f);
  CHECK(0xcc,0,0x11); /* Selected node has neither TX-EOT append nor RX-EOT ignore. */
  CHECK(0xb0,0,7); /* lane_map_0123 */
  CHECK(0x40,8,0xf0111f); /* RGB888 command format, no interleave or bit swaps */
  CHECK(0x1b8,CMDBIT(16),CMDBIT(16));CHECK(0x44,0x13c2c,0x1ffff);
  CHECK(0xbc,0x3fd08,0xffffffffU);
  CHECK(0x2a8,(0x39U<<8)|((D->Plan.PacketsPerLine-1)<<6)|
      (D->Plan.EndOfLineBytes<<4)|1,0xffff);
  CHECK(0x2ac,D->Plan.Dsc.ChunkBytes,0xffff);
  CHECK(0x58,((D->Plan.PacketBytes+1)<<16)|0x39,0xffff033fU);
  CHECK(0x60,((D->Plan.PacketBytes+1)<<16)|0x39,0xffff033fU);
  CHECK(0x5c,(D->Plan.Config.Height<<16)|D->Plan.PayloadWidth,0xffffffffU);
  CHECK(0x64,(D->Plan.Config.Height<<16)|D->Plan.PayloadWidth,0xffffffffU);
  PUT(0x110,0); /* Command DMA is bounded/polled; frame IRQ belongs to DPU. */
  TRY(CmdChecked(D,HOST,4,0x1f5,0x1f7)); /* all lanes + CMD + controller */
  PUT(0x11c,0x23f); return CmdFence(D,HOST);
}

CR_STATUS CmdPacket(CR_MDSS_CMD *D, UINT8 Type, CONST UINT8 *Payload,
                    UINTN Bytes, BOOLEAN LowPower)
{
  CR_STATUS S; UINT32 L,Interrupt,V; BOOLEAN Long;
  CR_MDSS_CMD_MEMORY *M=&D->Commands; UINT8 *P=M->Cpu;
  if(!Payload||!Bytes||Bytes>252||LowPower>1)return CR_INVALID_PARAMETER;
  Long=Type==0x0a||Type==0x39;
  if(!Long&&Type!=0x05&&Type!=0x15&&Type!=0x07)return CR_UNSUPPORTED;
  if((Type==0x05&&Bytes!=1)||((Type==0x15||Type==7)&&Bytes!=2)||
     (Type==0x0a&&Bytes!=128))return CR_INVALID_PARAMETER;
  if(!D->HostTouched||D->CommandInFlight)return CR_NOT_READY;
  L=Long?(UINT32)((Bytes+7)&~3U):4;
  if(!P||M->Bytes<L)return CR_BAD_BUFFER_SIZE;
  TRY(CmdDmaReady(D,HOST));
  TRY(CmdPoll(D,HOST,8,CMDBIT(1),0,5000));
  cr_memset(P,0xff,L);P[0]=Long?(UINT8)Bytes:Payload[0];
  P[1]=Long?(UINT8)(Bytes>>8):Bytes==2?Payload[1]:0;
  P[2]=Type;P[3]=Long?0xc0:0x80;
  if(Long)cr_memcpy(P+4,Payload,Bytes);
  TRY(M->Publish(M->Context,P,L));
  TRY(CmdRead(D,HOST,0x110,&Interrupt));
  if(Interrupt&CMDBIT(24))return CR_DEVICE_ERROR;
  Interrupt&=0xaaaaaaaaU;
  PUT(0x110,(Interrupt&~CMDBIT(1))|1); TRY(CmdFence(D,HOST));
  TRY(CmdPoll(D,HOST,0x110,1,0,1000)); /* fresh completion only */
  PUT(0x180,0); /* Real memory DMA, including full 128-byte PPS. */
  PUT(0x3c,CMDBIT(28)|(LowPower?CMDBIT(26):0));
  TRY(CmdUpdate(D,HOST,0x50,CMDBIT(20)|CMDBIT(16),CMDBIT(20)|CMDBIT(16)));
  CHECK(0x48,(UINT32)M->DeviceAddress,0xffffffffU);CHECK(0x4c,L,0xffffff);TRY(CmdFence(D,HOST));
  TRY(CmdDmaReady(D,HOST));
  /* A failed trigger write may already have reached hardware. Retain mapping
   * until a fresh completion AND DMA-idle, or the explicit Stop fence. */
  D->CommandInFlight=TRUE;
  PUT(0x90,1);TRY(CmdFence(D,HOST));
  TRY(CmdPoll(D,HOST,0x110,1,1,5000));
  TRY(CmdRead(D,HOST,0x110,&V));if(V&CMDBIT(24))return CR_DEVICE_ERROR;
  TRY(CmdPoll(D,HOST,8,CMDBIT(1),0,5000));
  PUT(0x110,(Interrupt&~CMDBIT(1))|1);TRY(CmdFence(D,HOST));
  PUT(0x110,Interrupt);TRY(CmdFence(D,HOST));
  D->CommandInFlight=FALSE;return CR_SUCCESS;
}

CR_STATUS CmdHostStop(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  if(!D->HostTouched)return D->CommandInFlight?CR_NOT_READY:CR_SUCCESS;
  /* DPU must already be fenced, so no new MDP transfer can start. Keep host
   * clocks and command buffer mapping until both engines are visibly idle. */
  if(D->FrameInFlight||D->DpuTouched)return CR_NOT_READY;
  PUT(0x11c,FORCE_CLOCKS);TRY(CmdFence(D,HOST));
  TRY(CmdPoll(D,HOST,8,CMDBIT(1)|CMDBIT(2),0,50000));
  TRY(CmdUpdate(D,HOST,4,7,0));TRY(CmdFence(D,HOST));
  TRY(CmdPoll(D,HOST,4,7,0,1000));
  TRY(CmdPoll(D,HOST,8,CMDBIT(1)|CMDBIT(2),0,5000));
  PUT(0x110,0);PUT(0x11c,0);TRY(CmdFence(D,HOST));
  D->CommandInFlight=FALSE;D->HostTouched=FALSE;return CR_SUCCESS;
}
