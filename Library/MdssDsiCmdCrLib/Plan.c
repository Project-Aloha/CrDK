/* Command bandwidth for the qualified DPU6.0/DSI2.4/DPHY4.1 backend.
 * Derived from Qualcomm dsi_panel_calc_dsi_transfer_time (SM8250).
 * Copyright (c) 2015-2020 The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"

STATIC CR_STATUS PanelData(CONST CR_MDSS_CMD_PROFILE *F)
{
  if(!F->ResetCount||F->ResetCount>CR_MDSS_CMD_RESET_MAX||
     !F->OnCount||F->OnCount>CR_MDSS_CMD_ON_MAX||
     !F->OffCount||F->OffCount>CR_MDSS_CMD_OFF_MAX||
     F->OffResetDelayUs>120000||F->BrightnessMsbFirst>1)return CR_INVALID_PARAMETER;
  if(!F->Reset[F->ResetCount-1].High)return CR_UNSUPPORTED;
  for(UINT32 I=0;I<F->ResetCount;++I)
    if(F->Reset[I].High>1||F->Reset[I].DelayUs>120000)return CR_INVALID_PARAMETER;
  if(F->BrightnessMaximum&&(F->BrightnessBytes<1||F->BrightnessBytes>2||
     !F->BrightnessCommand||F->BrightnessMaximum>(F->BrightnessBytes==1?255U:65535U)))
    return CR_INVALID_PARAMETER;
  for(UINT32 Group=0;Group<2;++Group){
    CONST CR_MDSS_CMD_PACKET *Packets=Group?F->Off:F->On;
    UINT32 Count=Group?F->OffCount:F->OnCount;
    for(UINT32 I=0;I<Count;++I){
      CONST CR_MDSS_CMD_PACKET *P=&Packets[I];
      if(!P->Bytes||P->Bytes>CR_MDSS_CMD_PAYLOAD_MAX||P->LowPower>1||P->DelayUs>120000)
        return CR_INVALID_PARAMETER;
      if(P->Type!=0x39&&P->Type!=0x05&&P->Type!=0x15&&P->Type!=0x07)return CR_UNSUPPORTED;
      if((P->Type==0x05&&P->Bytes!=1)||((P->Type==0x15||P->Type==0x07)&&P->Bytes!=2))
        return CR_INVALID_PARAMETER;
    }
  }
  return CR_SUCCESS;
}
CR_STATUS CrMdssCmdBuildPlanProfile(CONST CR_MDSS_CMD_PROFILE *F, CR_MDSS_CMD_PLAN *Out)
{
  CR_MDSS_CMD_PLAN P;CR_STATUS S;UINT64 BitsPerLine,Rate,FrameBits,Htotal,Vtotal;
  STATIC CONST UINT32 Timings[14]={0,16,4,4,30,30,4,4,3,2,4,0,16,20};
  if(!F||!Out||F->Revision!=1||F->Config.Revision!=1)return CR_INVALID_PARAMETER;
  CONST CR_MDSS_CMD_CAPABILITIES *Hw=&F->Hardware;
  CONST CR_MDSS_CMD_CONFIG *C=&F->Config;
  if(Hw->Soc!=CR_MDSS_CMD_SM8250||C->Soc!=Hw->Soc||Hw->DpuMajor!=6||Hw->DpuMinor||
     Hw->DsiMajor!=2||Hw->DsiMinor!=4||Hw->PhyMajor!=4||Hw->PhyMinor!=1||
     Hw->Flags!=(CR_MDSS_CMD_CAP_COMMAND|CR_MDSS_CMD_CAP_DPHY|CR_MDSS_CMD_CAP_LEGACY_DSC))
    return CR_UNSUPPORTED;
  /* This sequence has one qualified rate/timing pair. Catalog capabilities
   * do not authorize arbitrary analogue programming or unqualified rates. */
  if(C->Controller||C->Lanes!=4||C->LaneBitHz!=327096544ULL||C->VsyncHz!=19200000||
     cr_memcmp(F->PhyTimings,Timings,sizeof(Timings)))return CR_UNSUPPORTED;
  if(!C->Width||C->Width>2560||!C->Height||C->Height>0xffeb||
     C->Stride<(UINT64)C->Width*4||(C->Stride&3)||C->Stride>65532||
     !C->RefreshHz||C->RefreshHz>120||!C->JitterNumerator||!C->JitterDenominator||
     !C->FrameThresholdUs)return CR_INVALID_PARAMETER;
  Htotal=(UINT64)C->Width+C->HSync+C->HBackPorch+C->HFrontPorch;
  Vtotal=(UINT64)C->Height+C->VSync+C->VBackPorch+C->VFrontPorch;
  if(Htotal>65535||Vtotal>0xfff0||!Htotal||!Vtotal||
     C->VsyncHz/(Vtotal*C->RefreshHz)>0xfffff)return CR_UNSUPPORTED;
  if(C->Dsc.Revision!=1||C->Dsc.MinorVersion!=1||C->Dsc.ScrVersion||
     C->Dsc.BitsPerComponent!=8||C->Dsc.BppQ4!=128||
     C->Dsc.PictureWidth!=C->Width||C->Dsc.PictureHeight!=C->Height||
     !C->Dsc.SliceWidth||C->Width!=2U*C->Dsc.SliceWidth||
     !C->Dsc.SliceHeight||C->Height%C->Dsc.SliceHeight||C->Dsc.BlockPrediction!=TRUE)
    return CR_UNSUPPORTED;
  S=PanelData(F);if(S!=CR_SUCCESS)return S;
  cr_memset(&P,0,sizeof(P));P.Config=*C;P.Hardware=*Hw;
  S=CrMdssDscBuild(&C->Dsc,&P.Dsc);if(S!=CR_SUCCESS)return S;
  P.Htotal=(UINT32)Htotal;P.Vtotal=(UINT32)Vtotal;P.FrameUs=1000000U/C->RefreshHz;
  /* Android command payload includes 56 bits per slice packet. Video blanking
   * is not transmitted, and panel geometry is data rather than board identity. */
  BitsPerLine=((UINT64)C->Dsc.SliceWidth*(C->Dsc.BppQ4>>4)+56)*P.Dsc.SliceCount/C->Lanes;
  P.MinimumLaneBitHz=BitsPerLine*C->Height*C->RefreshHz;
  Rate=C->LaneBitHz;FrameBits=P.MinimumLaneBitHz*P.FrameUs;
  P.TransferUs=(UINT32)((FrameBits+Rate-1)/Rate);
  if(P.TransferUs>=P.FrameUs||P.FrameUs-P.TransferUs<C->FrameThresholdUs)return CR_UNSUPPORTED;
  P.GuardUs=P.FrameUs-P.TransferUs;P.LaneBitHz=Rate;P.ByteHz=Rate/8;P.ByteInterfaceHz=Rate/16;
  P.PllPostDivider=2;P.PhyPostDivider=1;P.PixelDivider=3;P.VcoHz=Rate*2;P.PixelHz=Rate/6;
  P.PacketBytes=P.Dsc.ChunkBytes;P.PacketsPerLine=P.Dsc.SliceCount;
  P.PayloadWidth=(P.PacketBytes*P.PacketsPerLine+2)/3;
  P.EndOfLineBytes=P.PacketBytes*P.PacketsPerLine%3;
  cr_memcpy(P.PhyTimings,F->PhyTimings,sizeof(P.PhyTimings));
  P.HostClockTiming=P.PhyTimings[12]|(P.PhyTimings[13]<<8);
  *Out=P;return CR_SUCCESS;
}
CR_STATUS CrMdssCmdBuildPlan(CONST CR_MDSS_CMD_CONFIG *C, CR_MDSS_CMD_PLAN *Out)
{
  if(!C||!Out||C->Revision!=1)return CR_INVALID_PARAMETER;
  /* Legacy callers retain the original reference-panel admission. New native
   * callers supply their real provider profile instead of using this wrapper. */
  if(C->Soc!=CR_MDSS_CMD_SM8250||C->Controller||C->Lanes!=4||C->Width!=1080||
     C->Height!=2340||C->RefreshHz!=60||C->HSync!=1||C->HBackPorch!=40||C->HFrontPorch!=95||
     C->VSync!=1||C->VBackPorch!=4||C->VFrontPorch!=25||C->JitterNumerator!=4||
     C->JitterDenominator!=1||C->FrameThresholdUs!=800||C->Dsc.SliceWidth!=540||
     C->Dsc.SliceHeight!=20)return CR_UNSUPPORTED;
  CR_MDSS_CMD_PROFILE F;CrMdssCmdR66451Profile(&F);F.Config=*C;
  return CrMdssCmdBuildPlanProfile(&F,Out);
}
CR_STATUS CmdPlanCheck(CR_MDSS_CMD *D)
{
  CR_MDSS_CMD_PLAN P;
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  CR_STATUS S=CrMdssCmdBuildPlanProfile(&D->Profile,&P);if(S!=CR_SUCCESS)return S;
  return cr_memcmp(&P,&D->Plan,sizeof(P))?CR_INVALID_PARAMETER:CR_SUCCESS;
}
