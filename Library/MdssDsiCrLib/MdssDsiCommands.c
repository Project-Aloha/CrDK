/* DSI6G embedded command DMA/FIFO and DSC PPS. Android dsi_ctrl_hw_cmn.c,
 * mainline dsi_host.c packet format. SPDX-License-Identifier: GPL-2.0-only */
#include "MdssDsiInternal.h"
#include <oskal/cr_string.h>
CR_STATUS CrMdssDsiSetCommandMemory(CR_MDSS_DSI *D,CONST CR_MDSS_DSI_COMMAND_MEMORY *M)
{
 if(!D||!D->Initialized||!M||!M->Cpu||!M->Ready||!M->Publish||M->Bytes<132||
    M->Bytes>(UINTN)-1-(UINTN)M->Cpu||!M->DeviceAddress||(M->DeviceAddress&7)||
    M->DeviceAddress>CR_MAX_UINT32||M->Bytes-1>CR_MAX_UINT32-M->DeviceAddress)
  return CR_INVALID_PARAMETER;
 if(D->Prepared||D->CleanupPending||D->CommandsConfigured)return CR_NOT_READY;
 D->CommandMemory=*M;D->CommandsConfigured=TRUE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiWritePacket(CR_MDSS_DSI *D,UINT32 C,UINT8 Type,
    CONST UINT8 *Payload,UINTN Bytes,BOOLEAN LowPower)
{
 UINT8 Packet[256];UINT32 I,L,Ctrl,Interrupt,Value;BOOLEAN Long;CR_STATUS S;
 CR_MDSS_EXT_WINDOW W;CR_MDSS_DSI_COMMAND_MEMORY *M;
 if(!D||!D->Initialized||LowPower>1||C<D->Plan.Mode.FirstController||
    C>=D->Plan.Mode.FirstController+D->Plan.Mode.Controllers||!Payload||
    !Bytes||Bytes>252||Bytes>(UINTN)-1-(UINTN)Payload)return CR_INVALID_PARAMETER;
 Long=Type==0x0a||Type==0x29||Type==0x39;
 if(!Long&&Type!=0x03&&Type!=0x13&&Type!=0x23&&Type!=0x05&&Type!=0x15&&Type!=0x07)
  return CR_UNSUPPORTED;
 if((Type==0x05||Type==0x13)&&Bytes!=1)return CR_INVALID_PARAMETER;
 if((Type==0x23||Type==0x15||Type==0x07)&&Bytes!=2)return CR_INVALID_PARAMETER;
 if(Type==0x03)return CR_UNSUPPORTED; /* zero-payload generic not exposed */
 if(Type==0x0a&&Bytes!=128)return CR_INVALID_PARAMETER;
 if(!D->Prepared||D->Active||D->CleanupPending)return CR_NOT_READY;
 I=C-D->Plan.Mode.FirstController;W=XHost(C);M=&D->CommandMemory;
 L=Long?(UINT32)((Bytes+7)&~3U):4;
 if(L>64&&(!D->CommandsConfigured||M->Bytes<L))return CR_NOT_READY;
 S=XReady(D,W);if(S!=CR_SUCCESS)return S;
 if(L>64){S=XExact(M->Ready(M->Context,C));if(S!=CR_SUCCESS)return S;}
 cr_memset(Packet,0xff,L);Packet[0]=Long?(UINT8)Bytes:Payload[0];
 Packet[1]=Long?(UINT8)(Bytes>>8):Bytes==2?Payload[1]:0;
 Packet[2]=(UINT8)(Type|(D->Plan.Link[I].VirtualChannel<<6));Packet[3]=Long?0xc0:0x80;
 if(Long)cr_memcpy(Packet+4,Payload,Bytes);
 if(L>64){cr_memcpy(M->Cpu,Packet,L);S=XExact(M->Publish(M->Context,M->Cpu,L));if(S!=CR_SUCCESS)return S;}
 S=XRead(D,W,4,&Ctrl);if(S!=CR_SUCCESS)return S;
 S=XRead(D,W,0x110,&Interrupt);if(S!=CR_SUCCESS)return S;
 if(Interrupt&XB(24))return CR_DEVICE_ERROR;
 Interrupt&=0xaaaaaaaaU;D->CommandTouched[I]=TRUE;D->CleanupPending=TRUE;
#define CPUT(o,v) do{S=XWrite(D,W,(o),(v));if(S!=CR_SUCCESS)return S;}while(0)
 /* Mask IRQ delivery, clear only stale DMA_DONE; all other status is retained. */
 CPUT(0x110,(Interrupt&~XB(1))|1);S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 S=XPoll(D,W,0x110,1,0,1000);if(S!=CR_SUCCESS)return S;
 S=XChecked(D,W,4,Ctrl|XB(2),XB(2));if(S!=CR_SUCCESS)return S;
 if(L<=64){
  CPUT(0x180,XB(1)|XB(2)|(3U<<16));
  for(UINT32 J=0;J<L;J+=4){Value=(UINT32)Packet[J]|((UINT32)Packet[J+1]<<8)|
    ((UINT32)Packet[J+2]<<16)|((UINT32)Packet[J+3]<<24);CPUT(0x17c,Value);}
  if((L/4)&1)CPUT(0x17c,0);
 }else{
  CPUT(0x180,0);CPUT(0x48,(UINT32)M->DeviceAddress);
 }
 /* No broadcast: send and observe each actual host independently. */
 CPUT(0x3c,XB(28)|(LowPower?XB(26):0));CPUT(0x4c,L);
 S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 if(L>64){S=XExact(M->Ready(M->Context,C));if(S!=CR_SUCCESS)return S;}
 CPUT(0x90,1);S=XFence(D,W);if(S!=CR_SUCCESS)return S;
 S=XPoll(D,W,0x110,1,1,5000);if(S!=CR_SUCCESS)return S;
 S=XRead(D,W,0x110,&Value);if(S!=CR_SUCCESS)return S;
 if(Value&XB(24))return CR_DEVICE_ERROR;
 /* Completion status alone cannot certify that the DMA engine is idle. */
 S=XPoll(D,W,8,XB(1),0,5000);if(S!=CR_SUCCESS)return S;
 CPUT(0x110,(Interrupt&~XB(1))|1);CPUT(0x180,0);CPUT(0x1ec,1);
 S=XDelay(D,W,1);if(S!=CR_SUCCESS)return S;CPUT(0x1ec,0);
 S=XChecked(D,W,4,Ctrl,7);if(S!=CR_SUCCESS)return S;
 CPUT(0x110,Interrupt);S=XFence(D,W);if(S!=CR_SUCCESS)return S;
#undef CPUT
 D->CommandTouched[I]=FALSE;D->CleanupPending=FALSE;return CR_SUCCESS;
}
CR_STATUS CrMdssDsiSendPps(CR_MDSS_DSI *D,UINT8 Identifier)
{
 UINT8 Pps[128],Compression[2]={1,0};UINT32 I;CR_STATUS S;
 if(!D||!D->Initialized||!D->Plan.Mode.Dsc)return CR_INVALID_PARAMETER;
 if(!D->Prepared||D->Active||D->CleanupPending)return CR_NOT_READY;
 S=CrMdssDscPackPps(&D->Plan.Mode.Compression,Identifier,Pps,sizeof(Pps));if(S!=CR_SUCCESS)return S;
 for(I=0;I<D->Plan.Mode.Controllers;++I){
  if(!D->PpsSent[I]){S=CrMdssDsiWritePacket(D,D->Plan.Link[I].Controller,0x0a,Pps,sizeof(Pps),TRUE);
   if(S!=CR_SUCCESS)return S;D->PpsSent[I]=TRUE;}
  if(!D->CompressionEnabled[I]){S=CrMdssDsiWritePacket(D,D->Plan.Link[I].Controller,7,Compression,2,TRUE);
   if(S!=CR_SUCCESS)return S;D->CompressionEnabled[I]=TRUE;}
 }
 return CR_SUCCESS;
}
