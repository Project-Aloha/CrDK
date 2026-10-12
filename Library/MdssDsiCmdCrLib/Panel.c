/* Provider-selected command panel sequence and retained physical cleanup.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
STATIC CR_STATUS Send(CR_MDSS_CMD *D,CONST CR_MDSS_CMD_PACKET *C,UINT32 Count)
{
  for(UINT32 I=0;I<Count;++I){
    CR_STATUS S=CmdPacket(D,C[I].Type,C[I].Payload,C[I].Bytes,C[I].LowPower);
    if(S!=CR_SUCCESS)return S;
    if(C[I].DelayUs){S=CmdDelay(D,CrMdssCmdHost,C[I].DelayUs);if(S!=CR_SUCCESS)return S;}
  }
  return CR_SUCCESS;
}
CR_STATUS CmdPanelOn(CR_MDSS_CMD *D)
{
  CR_STATUS S;UINT8 Pps[128];
  S=CmdPlanCheck(D);if(S!=CR_SUCCESS)return S;
  if(D->PanelTouched||!D->PanelOwned||!D->HostTouched)return CR_NOT_READY;
  D->PanelTouched=TRUE;
  for(UINT32 I=0;I<D->Profile.ResetCount;++I){
    S=D->Io.PanelReset(D->Io.Context,D->Profile.Reset[I].High);if(S!=CR_SUCCESS)return S;
    if(D->Profile.Reset[I].DelayUs){S=CmdDelay(D,CrMdssCmdHost,D->Profile.Reset[I].DelayUs);if(S!=CR_SUCCESS)return S;}
  }
  S=Send(D,D->Profile.On,D->Profile.OnCount);if(S!=CR_SUCCESS)return S;
  /* PPS follows the selected ON program and uses the same real DMA owner. */
  S=CrMdssDscPackPps(&D->Plan.Dsc,0,Pps,sizeof(Pps));if(S!=CR_SUCCESS)return S;
  return CmdPacket(D,0x0a,Pps,sizeof(Pps),TRUE);
}
CR_STATUS CmdPanelOff(CR_MDSS_CMD *D)
{
  CR_STATUS S=CR_SUCCESS,R;
  if(!D->PanelTouched)return CR_SUCCESS;
  if(D->FrameInFlight||D->DpuTouched)return CR_NOT_READY;
  if(D->HostTouched&&!D->CommandInFlight)
    S=Send(D,D->Profile.Off,D->Profile.OffCount);
  /* Reset is required even after a failed packet; logical SWIRE withdrawal
   * does not fence the physical panel or the independent host command DMA. */
  R=D->Io.PanelReset(D->Io.Context,FALSE);if(R!=CR_SUCCESS)return R;
  if(D->Profile.OffResetDelayUs){R=CmdDelay(D,CrMdssCmdHost,D->Profile.OffResetDelayUs);if(R!=CR_SUCCESS)return R;}
  D->PanelTouched=FALSE;return S;
}
CR_STATUS CrMdssCmdSetBrightness(CR_MDSS_CMD *D,UINT32 Level)
{
  UINT8 P[3];CR_STATUS S;
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  if(!D->Profile.BrightnessMaximum)return CR_UNSUPPORTED;
  if(Level>D->Profile.BrightnessMaximum)return CR_INVALID_PARAMETER;
  if(D->Frozen||!D->Active||D->CleanupPending||D->Faulted)return CR_NOT_READY;
  S=CmdPlanCheck(D);if(S!=CR_SUCCESS)return S;
  P[0]=D->Profile.BrightnessCommand;
  P[1]=(UINT8)(D->Profile.BrightnessBytes==2&&D->Profile.BrightnessMsbFirst?Level>>8:Level);
  P[2]=(UINT8)(D->Profile.BrightnessMsbFirst?Level:Level>>8);
  S=CmdPacket(D,0x39,P,1+D->Profile.BrightnessBytes,TRUE);
  if(S!=CR_SUCCESS&&D->CommandInFlight){
    D->Faulted=TRUE;D->CleanupPending=TRUE;D->Active=FALSE;
  }
  return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;
}
