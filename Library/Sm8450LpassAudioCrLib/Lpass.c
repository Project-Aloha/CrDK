/* SPDX-License-Identifier: GPL-2.0-only
 * Copyright (c) 2026 Project Aloha Authors.
 * Exact SM8450/HDK pins and macro sequence from OnePlus audio-kernel
 * 877203a7cf9b7997, waipio DT and lpass-cdc-{rx,va,wsa}-macro/clk-rsc. */
#include <Library/Sm8450LpassAudioCrLib.h>
CONST CR_LPASS_WINDOW_TARGET CrSm8450LpassWindows[CR_LPASS_WINDOWS]={
 {0x03200000,0x1000},{0x033f0000,0x1000},{0x03240000,0x1000},
 {0x03440000,0x20000},{0x034da000,0x1000},
 {0x03210000,0x2000},{0x033b0000,0x2000},{0x03250000,0x2000},
 {0x033a40d8,4},{0x03420000,4},{0x033a20e0,4}
};
static BOOLEAN Valid(CONST CR_SM8450_LPASS *P){return P&&P->Self==P;}
static CR_STATUS Power(CR_SM8450_LPASS *P,UINT32 Mask){
 return P->Io.CheckPower?P->Io.CheckPower(P->Io.Context,Mask):CrAudioPrmCheck(P->Power,Mask);
}
static CR_STATUS Read(CR_SM8450_LPASS *P,UINT32 W,UINT32 O,UINT32 *V){
 if(W>=CR_LPASS_WINDOWS||(O&3)||O>CrSm8450LpassWindows[W].Span-4)return CR_INVALID_PARAMETER;
 return P->Io.Read32(P->Io.Context,&P->Lease,W,O,V);
}
static CR_STATUS Write(CR_SM8450_LPASS *P,UINT32 W,UINT32 O,UINT32 V){return P->Io.Write32(P->Io.Context,&P->Lease,W,O,V);}
static CR_STATUS Fault(CR_SM8450_LPASS *P,CR_STATUS S){P->Ready=FALSE;if(P->Error==CR_SUCCESS)P->Error=S;return S;}
/* Keep a journal entry before the first write, including unknown write errors.
 * Repeated writes to a field have one original snapshot and exact union mask. */
static CR_STATUS Update(CR_SM8450_LPASS *P,UINT32 W,UINT32 O,UINT32 Mask,UINT32 Value){
 UINT32 Old,V;CR_STATUS S=Read(P,W,O,&Old);if(S!=CR_SUCCESS)return S;
 UINT32 I=0;for(;I<P->Count;++I)if(P->Journal[I].Window==W&&P->Journal[I].Offset==O)break;
 if(I==P->Count){if(P->Count==CR_LPASS_JOURNAL)return CR_OUT_OF_RESOURCES;P->Journal[P->Count++]=(CR_LPASS_REGISTER){W,O,Mask,Old};}
 else {P->Journal[I].Before=(P->Journal[I].Before&P->Journal[I].Mask)|(Old&~P->Journal[I].Mask);P->Journal[I].Mask|=Mask;}
 S=Write(P,W,O,(Old&~Mask)|(Value&Mask));if(S!=CR_SUCCESS)return S;
 S=Read(P,W,O,&V);if(S!=CR_SUCCESS)return S;
 return (V&Mask)==(Value&Mask)?CR_SUCCESS:CR_DEVICE_ERROR;
}
CR_STATUS CrSm8450LpassInit(CR_SM8450_LPASS *P,CONST CR_LPASS_IO *Io,CR_AUDIO_PRM *Power){
 if(!P||P->Self||!Io||!Io->Context||!Io->Acquire||!Io->Read32||!Io->Write32||!Io->AllDrained||!Io->Release||
    (Power&&Power->Self!=Power)||(!Power&&!Io->CheckPower)||(Power&&Io->CheckPower))return CR_INVALID_PARAMETER;
 *P=(CR_SM8450_LPASS){.Self=P,.Io=*Io,.Power=Power};return CR_SUCCESS;
}
CR_STATUS CrSm8450LpassPrepare(CR_SM8450_LPASS *P){
 if(!Valid(P))return CR_INVALID_PARAMETER;
 if(P->Stopping||P->LeaseUncertain||P->Error!=CR_SUCCESS)return CR_BUSY;
 CR_STATUS S=Power(P,CR_AUDIO_PRM_ALL);if(S!=CR_SUCCESS)return S;
 if(P->Ready)return CR_SUCCESS;
 S=P->Io.Acquire(P->Io.Context,P,&P->Lease);
 if((P->Lease.Provider==NULL)!=(P->Lease.Token==0)||(S==CR_SUCCESS&&!P->Lease.Provider)){P->LeaseUncertain=TRUE;return Fault(P,CR_DEVICE_ERROR);}
 if(S!=CR_SUCCESS)return S;
 /* Exclusive ownership alone does not authorize resetting inherited playback.
  * Reject any active macro before changing pads or any macro register. */
 CONST CR_LPASS_REGISTER Inactive[]={
  {CrLpassRxMacro,0x100,3,0},{CrLpassRxMacro,0x104,1,0},{CrLpassRxMacro,0x108,3,0},
  {CrLpassVaMacro,0,1,0},{CrLpassVaMacro,4,1,0},{CrLpassVaMacro,8,3,0},
  {CrLpassWsaMacro,0,1,0},{CrLpassWsaMacro,4,1,0},{CrLpassWsaMacro,8,3,0}
 };
 for(UINT32 I=0;I<sizeof(Inactive)/sizeof(Inactive[0]);++I){UINT32 V;
  S=Read(P,Inactive[I].Window,Inactive[I].Offset,&V);if(S!=CR_SUCCESS)return Fault(P,S);
  if(V&Inactive[I].Mask)return Fault(P,CR_BUSY);
 }
 /* Default TX-derived clocks use mux0. Observe inherited selection; changing
  * to/from native mux1 requires an independent coupled-clock transition. */
 for(UINT32 I=CrLpassRxMux;I<=CrLpassWsaMux;++I){UINT32 V;
  S=Read(P,I,0,&V);if(S!=CR_SUCCESS)return Fault(P,S);
  if(V!=0)return Fault(P,CR_UNSUPPORTED);
 }
 /* LPI is independent of TLMM. Required HDK pads: RX3/4/5 func1,
  * TX0/1/2/14 func1, WSA10/11 func2. 2mA encodes0. */
 CONST struct {UINT8 Pin,Function,Pull,Slew;} Pins[]={
  {3,1,0,8},{4,1,2,10},{5,1,2,12},{0,1,0,0},
  {1,1,2,2},{2,1,2,4},{14,1,2,6},{10,2,0,16},{11,2,2,18}
 };
 for(UINT32 I=0;I<sizeof(Pins)/sizeof(Pins[0]);++I){
  S=Update(P,CrLpassLpi,Pins[I].Pin*0x1000U,0x3ff,(Pins[I].Function<<2)|Pins[I].Pull);if(S!=CR_SUCCESS)return Fault(P,S);
  S=Update(P,CrLpassLpi,Pins[I].Pin*0x1000U+4,2,0);if(S!=CR_SUCCESS)return Fault(P,S);
  S=Update(P,CrLpassSlew,0,3U<<Pins[I].Slew,1U<<Pins[I].Slew);if(S!=CR_SUCCESS)return Fault(P,S);
 }
 /* Frame-generator virtual offsets0x3000/3004/3080 are VA physical offsets,
  * not TX. Preserve the DT reset pulse and final fs-gating selection. */
 CONST CR_LPASS_REGISTER Seq[]={
  {CrLpassVaMacro,0,1,1},{CrLpassVaMacro,4,3,3},{CrLpassVaMacro,4,3,1},{CrLpassVaMacro,0x80,2,2},
  {CrLpassRxMacro,0x100,1,1},{CrLpassRxMacro,0x100,2,2},{CrLpassRxMacro,0x104,2,0},{CrLpassRxMacro,0x104,1,1},
  {CrLpassWsaMacro,0x88,1,1},{CrLpassWsaMacro,0,1,1},{CrLpassWsaMacro,4,1,1},
  {CrLpassRxMacro,0x108,2,2},{CrLpassRxMacro,0x108,1,1},{CrLpassRxMacro,0x108,2,0},
  {CrLpassVaMacro,8,2,2},{CrLpassVaMacro,8,1,1},{CrLpassVaMacro,8,2,0},
  {CrLpassWsaMacro,8,2,2},{CrLpassWsaMacro,8,1,1},{CrLpassWsaMacro,8,2,0},{CrLpassWsaMacro,8,0x1c,0x0c}
 };
 for(UINT32 I=0;I<sizeof(Seq)/sizeof(Seq[0]);++I){S=Update(P,Seq[I].Window,Seq[I].Offset,Seq[I].Mask,Seq[I].Before);if(S!=CR_SUCCESS)return Fault(P,S);}
 P->Ready=TRUE;return CR_SUCCESS;
}
CR_STATUS CrSm8450LpassClockHz(CR_SM8450_LPASS *P,UINT32 Controller,UINT32 *Rate){
 if(!Valid(P)||Controller>2||!Rate)return CR_INVALID_PARAMETER;
 if(!P->Ready||P->Stopping||P->Error!=CR_SUCCESS)return CR_NOT_READY;
 CR_STATUS S=Power(P,3|(1U<<(Controller==0?CrPrmRx:Controller==1?CrPrmTx:CrPrmWsa)));
 if(S!=CR_SUCCESS)return Fault(P,S);
 UINT32 M,F,Swr,Mux,Freq=1;UINT32 W=Controller==0?CrLpassRxMacro:Controller==1?CrLpassVaMacro:CrLpassWsaMacro;
 UINT32 O=Controller==0?0x100:0;
 if((S=Read(P,W,O,&M))!=CR_SUCCESS||(S=Read(P,W,O+4,&F))!=CR_SUCCESS||
    (S=Read(P,W,O+8,&Swr))!=CR_SUCCESS||(S=Read(P,CrLpassRxMux+Controller,0,&Mux))!=CR_SUCCESS)return Fault(P,S);
 if(Controller==2&&(S=Read(P,W,0x88,&Freq))!=CR_SUCCESS)return Fault(P,S);
 if((M&(Controller==0?3:1))!=(Controller==0?3U:1U)||(F&3)!=1||
    (Swr&(Controller==2?0x1f:3))!=(Controller==2?0x0dU:1U)||!(Freq&1)||Mux)return Fault(P,CR_DEVICE_ERROR);
 *Rate=9600000;return CR_SUCCESS;
}
CR_STATUS CrSm8450LpassStop(CR_SM8450_LPASS *P){
 if(!Valid(P))return CR_INVALID_PARAMETER;if(P->LeaseUncertain)return CR_DEVICE_ERROR;
 P->Stopping=TRUE;P->Ready=FALSE;
 if(!P->Lease.Provider){P->Stopping=FALSE;P->Error=CR_SUCCESS;return CR_SUCCESS;}
 CR_STATUS S=P->Io.AllDrained(P->Io.Context,&P->Lease);if(S!=CR_SUCCESS)return S;
 if(P->Count){S=Power(P,CR_AUDIO_PRM_ALL);if(S!=CR_SUCCESS)return S;}
 while(P->Count){CR_LPASS_REGISTER *R=&P->Journal[P->Count-1];UINT32 V;
  S=Read(P,R->Window,R->Offset,&V);if(S!=CR_SUCCESS)return S;
  S=Write(P,R->Window,R->Offset,(V&~R->Mask)|(R->Before&R->Mask));if(S!=CR_SUCCESS)return S;
  S=Read(P,R->Window,R->Offset,&V);if(S!=CR_SUCCESS)return S;
  if((V&R->Mask)!=(R->Before&R->Mask))return CR_DEVICE_ERROR;
  --P->Count;
 }
 S=P->Io.Release(P->Io.Context,&P->Lease);
 if(S!=CR_SUCCESS){if(!P->Lease.Provider||!P->Lease.Token)P->LeaseUncertain=TRUE;return S;}
 if(P->Lease.Provider||P->Lease.Token){P->LeaseUncertain=TRUE;return CR_DEVICE_ERROR;}
 P->Stopping=FALSE;P->Error=CR_SUCCESS;return CR_SUCCESS;
}
