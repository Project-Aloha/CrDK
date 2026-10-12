/* Packaged XML mode/commands -> shared DSI/CPHY/dual/DSC panel caller.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "MdpExtendedPanel.h"
#include <Library/BaseMemoryLib.h>
#include <oskal/cr_status.h>
STATIC EFI_STATUS Sequence(CONST MDP_DSI_SEQUENCE *S,UINT32 Mask,CR_MDSS_PANEL_COMMAND *Out) {
  if(S->Unsupported)return EFI_UNSUPPORTED;
  if(S->Count>CR_MDSS_PANEL_MAX_COMMANDS || (S->Count&&!S->Commands))return EFI_BAD_BUFFER_SIZE;
  for(UINT32 I=0;I<S->Count;++I)Out[I]=(CR_MDSS_PANEL_COMMAND){S->Commands[I].DataType,
    S->Commands[I].DataType==0xff?0:(UINT8)Mask,
    TRUE,0,S->Commands[I].DelayMs,
    S->Commands[I].DataType==0xff?NULL:S->Commands[I].Payload,
    S->Commands[I].DataType==0xff?0:S->Commands[I].Length};
  return EFI_SUCCESS;
}
EFI_STATUS MdpBuildExtendedPanelForSoc(UINT32 Soc,CONST MDP_PANEL_CONFIG *P,CONST CR_MDSS_DSC_PARAMS *Dsc,
    CONST UINT8 LaneMap[4],BOOLEAN Continuous,CONST CR_MDSS_PANEL_RESET *Reset,UINT32 ResetCount,
    BOOLEAN OffResetHigh,MDP_EXTENDED_PANEL *Out) {
  if(!P||!LaneMap||!Out||Continuous>1||OffResetHigh>1)return EFI_INVALID_PARAMETER;
  CONST CR_MDSS_HW_PROFILE *Hardware=CrMdssGetHardwareProfile(Soc);
  if(!Hardware||!(Hardware->Capabilities&CR_MDSS_HW_VIDEO))return EFI_UNSUPPORTED;
  if(P->InterfaceType!=8||P->InterfaceColorFormat!=3||P->Timing.DsiColorFormat!=36||
     P->HasUnsupportedCommandMode||P->HasUnsupportedGeometry||P->HasUnsupportedPhy||P->SplitLink)
    return EFI_UNSUPPORTED;
  if((P->Dsc&&!Dsc)||(!P->Dsc&&Dsc))return EFI_INVALID_PARAMETER;
  MDP_EXTENDED_PANEL V;ZeroMem(&V,sizeof(V));CR_MDSS_DSI_MODE *M=&V.Profile.Mode;
  CONST EFI_MDP_DSI_TIMING *T=&P->Timing;
  M->Revision=CR_MDSS_DSI_REVISION;M->Controllers=P->ControllerCount;M->FirstController=T->DsiController;
  M->PhyType=P->Cphy?CrMdssCphy:CrMdssDphy;M->ContinuousClock=Continuous;M->Dsc=P->Dsc;
  CopyMem(M->LaneMap,LaneMap,4);
  M->Raster=(CR_MDSS_MODE){.Soc=Soc,.Controller=T->DsiController,
    .Width=T->Width,.Height=T->Height,.Stride=T->Width*4,
    .HSync=T->HSync,.HBackPorch=T->HBackPorch,.HFrontPorch=T->HFrontPorch,
    .VSync=T->VSync,.VBackPorch=T->VBackPorch,.VFrontPorch=T->VFrontPorch,
    .RefreshQ16=T->DsiRefreshRate,.Lanes=T->DsiLanes,.VirtualChannel=T->DsiVirtualChannel,
    .TrafficMode=T->DsiTrafficMode,.VideoFlags=T->DsiVideoFlags,.BorderColor=P->BorderColor,
    .InvertHsync=P->InvertHsyncPolarity,.InvertVsync=P->InvertVsyncPolarity,
    .HsaHseAfterVsVe=P->HsaHseAfterVsVe,.DisableEot=P->DisableEot};
  UINT64 H=(UINT64)T->Width+T->HSync+T->HBackPorch+T->HFrontPorch;
  UINT64 Y=(UINT64)T->Height+T->VSync+T->VBackPorch+T->VFrontPorch;
  if(H>0xffff||Y>0xffff||H*Y>MAX_UINT32||T->Width>0xffff)return EFI_INVALID_PARAMETER;
  M->PixelHz=(H*Y*T->DsiRefreshRate+32768)/65536;
  if(Dsc) {
    for(UINT32 I=0;I<ARRAY_SIZE(P->DscFieldsPresent);++I)if(I!=3&&!P->DscFieldsPresent[I])return EFI_COMPROMISED_DATA;
    if(P->DscMajor!=1||P->DscMinor!=Dsc->MinorVersion||P->DscScr!=Dsc->ScrVersion||
       P->DscSliceWidth!=Dsc->SliceWidth||P->DscSliceHeight!=Dsc->SliceHeight||
       Dsc->PictureWidth!=T->Width||Dsc->PictureHeight!=T->Height)return EFI_UNSUPPORTED;
    CR_STATUS S=CrMdssDscBuild(Dsc,&M->Compression);if(S!=CR_SUCCESS)return (EFI_STATUS)S;
    M->SlicesPerPacket=P->DscSlicesPerPacket;
  }
  if(M->Controllers<1||M->Controllers>2||M->FirstController>1||M->FirstController+M->Controllers>2)
    return EFI_INVALID_PARAMETER;
  UINT32 Mask=((1U<<M->Controllers)-1U)<<M->FirstController;
  EFI_STATUS E=Sequence(&P->InitSequence,Mask,V.On);if(E!=EFI_SUCCESS)return E;
  E=Sequence(&P->TermSequence,Mask,V.Off);if(E!=EFI_SUCCESS)return E;
  V.Profile.Revision=1;V.Profile.On=V.On;V.Profile.Off=V.Off;
  V.Profile.OnCount=(UINT32)P->InitSequence.Count;V.Profile.OffCount=(UINT32)P->TermSequence.Count;
  V.Profile.Reset=Reset;V.Profile.ResetCount=ResetCount;V.Profile.OffResetHigh=OffResetHigh;
  CR_STATUS S=CrMdssPanelProfileValidate(&V.Profile);if(S!=CR_SUCCESS)return (EFI_STATUS)S;
  *Out=V;Out->Profile.On=Out->On;Out->Profile.Off=Out->Off;return EFI_SUCCESS;
}
EFI_STATUS MdpBuildExtendedPanel(CONST MDP_PANEL_CONFIG *P,CONST CR_MDSS_DSC_PARAMS *Dsc,
    CONST UINT8 LaneMap[4],BOOLEAN Continuous,CONST CR_MDSS_PANEL_RESET *Reset,UINT32 ResetCount,
    BOOLEAN OffResetHigh,MDP_EXTENDED_PANEL *Out) {
  return MdpBuildExtendedPanelForSoc(CR_MDSS_SOC_SM8450,P,Dsc,LaneMap,Continuous,
    Reset,ResetCount,OffResetHigh,Out);
}
