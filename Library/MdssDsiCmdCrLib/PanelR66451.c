/* Exact HDK8245 selected R66451 panel transport data.
 * Source identity, original XML and hashes: Tests/Fixtures/Hdk8245Panel/README.md.
 * The transport implementation is GPL-2.0-only; raw board command bytes are
 * preserved from the supplied stock XML and independently matched to its FDT.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
typedef struct { UINT8 Type, Bytes; UINT16 DelayMs; UINT8 Payload[25]; } PANEL_COMMAND;
STATIC CONST PANEL_COMMAND On[]={
  {0x39,2,0,{0xb0,0x00}},
  {0x39,19,0,{0xd8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x5b,0x00,0x5b,0x00,0x5b,0x00,0x5b,0x00,0x5b}},
  {0x39,2,0,{0xb0,0x80}},
  {0x39,2,0,{0xe6,0x00}},
  {0x39,2,0,{0xb0,0x00}},
  {0x39,25,0,{0xcf,0x64,0x0b,0x00,0x00,0x00,0x00,0x00,0x00,0x08,0x00,0x0b,0x77,0x01,0x01,0x01,0x01,0x01,0x01,0x04,0x04,0x04,0x04,0x04,0x05}},
  {0x39,2,0,{0xb0,0x04}},
  {0x39,2,0,{0xf7,0x01}},
  {0x39,3,0,{0xdf,0x50,0x40}},
  {0x39,6,0,{0xf3,0x50,0x00,0x00,0x00,0x00}},
  {0x39,2,0,{0xf2,0x11}},
  {0x39,6,0,{0xf3,0x01,0x00,0x00,0x00,0x01}},
  {0x39,3,0,{0xf4,0x00,0x02}},
  {0x39,2,0,{0xf2,0x19}},
  {0x39,3,0,{0xdf,0x50,0x42}},
  {0x39,2,0,{0x35,0x00}},
  {0x39,5,0,{0x2a,0x00,0x00,0x04,0x37}},
  {0x39,5,0,{0x2b,0x00,0x00,0x09,0x23}},
  {0x05,1,120,{0x11}},
  {0x05,1,20,{0x29}},
};
STATIC CONST PANEL_COMMAND Off[]={
  {0x05,1,20,{0x28}},
  {0x05,1,120,{0x10}},
};
VOID CrMdssCmdHdk8245Config(CR_MDSS_CMD_CONFIG *C)
{
  if (!C) return;
  cr_memset(C,0,sizeof(*C));
  C->Revision=1; C->Soc=CR_MDSS_CMD_SM8250; C->Lanes=4;
  C->Width=1080; C->Height=2340; C->Stride=4320; C->RefreshHz=60;
  C->HSync=1; C->HBackPorch=40; C->HFrontPorch=95;
  C->VSync=1; C->VBackPorch=4; C->VFrontPorch=25;
  C->JitterNumerator=4; C->JitterDenominator=1; C->FrameThresholdUs=800;
  /* First exact dynamic bit-clock entry in the selected supplied FDT.
   * This is a requested rate, never a claim that a PLL is already running. */
  C->LaneBitHz=327096544ULL; C->VsyncHz=19200000;
  C->Dsc=(CR_MDSS_DSC_PARAMS){1,1,0,8,8U<<4,1080,2340,540,20,TRUE};
}

VOID CrMdssCmdR66451Profile(CR_MDSS_CMD_PROFILE *P)
{
  STATIC CONST UINT32 Timings[14]={0,16,4,4,30,30,4,4,3,2,4,0,16,20};
  if(!P)return;
  cr_memset(P,0,sizeof(*P));P->Revision=1;
  P->Hardware=(CR_MDSS_CMD_CAPABILITIES){CR_MDSS_CMD_SM8250,6,0,2,4,4,1,
    CR_MDSS_CMD_CAP_COMMAND|CR_MDSS_CMD_CAP_DPHY|CR_MDSS_CMD_CAP_LEGACY_DSC};
  CrMdssCmdHdk8245Config(&P->Config);
  cr_memcpy(P->PhyTimings,Timings,sizeof(Timings));
  P->ResetCount=3;
  P->Reset[0]=(CR_MDSS_CMD_RESET){TRUE,20000};
  P->Reset[1]=(CR_MDSS_CMD_RESET){FALSE,10000};
  P->Reset[2]=(CR_MDSS_CMD_RESET){TRUE,20000};
  P->OnCount=sizeof(On)/sizeof(On[0]);P->OffCount=sizeof(Off)/sizeof(Off[0]);
  for(UINT32 I=0;I<P->OnCount;++I){
    P->On[I].Type=On[I].Type;P->On[I].LowPower=TRUE;
    P->On[I].Bytes=On[I].Bytes;P->On[I].DelayUs=On[I].DelayMs*1000U;
    cr_memcpy(P->On[I].Payload,On[I].Payload,On[I].Bytes);
  }
  for(UINT32 I=0;I<P->OffCount;++I){
    P->Off[I].Type=Off[I].Type;P->Off[I].LowPower=FALSE;
    P->Off[I].Bytes=Off[I].Bytes;P->Off[I].DelayUs=Off[I].DelayMs*1000U;
    cr_memcpy(P->Off[I].Payload,Off[I].Payload,Off[I].Bytes);
  }
  P->OffResetDelayUs=20000;P->BrightnessMaximum=4095;
  P->BrightnessCommand=0x51;P->BrightnessBytes=2;P->BrightnessMsbFirst=TRUE;
}
