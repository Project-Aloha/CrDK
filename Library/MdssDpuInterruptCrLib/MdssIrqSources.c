/* SM8450 shared MDSS IRQ source observation. SPDX-License-Identifier: MIT */
#include <Library/MdssIrqSourcesCrLib.h>

typedef struct { CR_MDSS_IRQ_WINDOW Window; UINT32 Offset,Mask; } SOURCE;
#define IRQ_BIT(N) ((UINT32)1U<<(N))
/* Linux dpu_hw_interrupts.c (7xxx layout) and dpu_8_1_sm8450.h:
 * only INTF0..3 and TEAR1/2 exist on this target. Generic INTF4..8 and legacy
 * AD4 registers must not be accessed. DSI6G uses physical offset 110; Android
 * dsi_ctrl_hw_cmn_enable_status_interrupts also enables command streams0..2,
 * dynamic refresh and deskew, which the older mainline XML does not list.
 * DP packed mask bits are status+2, except PSR's independent mask register.
 * Android dp_hdcp2p2.c supplies STATUS2/3 HDCP masks absent in mainline.
 * Waipio LTM: MDSS-relative DSPP 55000/57000 + LTM 15300/14300 + enable50,
 * minus the DPU view's 1000 offset. Only WB/PB-done bits0/5 are defined.
 */
static CONST SOURCE Sources[]={
  {CrMdssIrqDpu,0x008U,0xffffffffU}, /* TOP0_2 */
  {CrMdssIrqDpu,0x01cU,0xffffffffU}, /* HIST */
  {CrMdssIrqDpu,0x341c0U,0xffffffffU},
  {CrMdssIrqDpu,0x351c0U,0xffffffffU},
  {CrMdssIrqDpu,0x361c0U,0xffffffffU},
  {CrMdssIrqDpu,0x371c0U,0xffffffffU},
  {CrMdssIrqDpu,0x35800U,0xffffffffU},
  {CrMdssIrqDpu,0x36800U,0xffffffffU},
  {CrMdssIrqDpu,0x69350U,IRQ_BIT(0)|IRQ_BIT(5)},
  {CrMdssIrqDpu,0x6a350U,IRQ_BIT(0)|IRQ_BIT(5)},
  {CrMdssIrqDsi0,0x110U,IRQ_BIT(1)|IRQ_BIT(9)|IRQ_BIT(11)|IRQ_BIT(13)|IRQ_BIT(15)|
                            IRQ_BIT(17)|IRQ_BIT(21)|IRQ_BIT(25)|IRQ_BIT(29)|IRQ_BIT(31)},
  {CrMdssIrqDsi1,0x110U,IRQ_BIT(1)|IRQ_BIT(9)|IRQ_BIT(11)|IRQ_BIT(13)|IRQ_BIT(15)|
                            IRQ_BIT(17)|IRQ_BIT(21)|IRQ_BIT(25)|IRQ_BIT(29)|IRQ_BIT(31)},
  {CrMdssIrqDpAhb,0x020U,IRQ_BIT(2)|IRQ_BIT(5)|IRQ_BIT(8)|IRQ_BIT(11)|IRQ_BIT(14)|
                             IRQ_BIT(17)|IRQ_BIT(20)|IRQ_BIT(23)|IRQ_BIT(26)|IRQ_BIT(29)},
  {CrMdssIrqDpAhb,0x024U,IRQ_BIT(2)|IRQ_BIT(5)|IRQ_BIT(8)|IRQ_BIT(11)|
                             IRQ_BIT(17)|IRQ_BIT(20)|IRQ_BIT(24)|IRQ_BIT(27)},
  {CrMdssIrqDpAhb,0x028U,IRQ_BIT(2)},
  {CrMdssIrqDpAhb,0x030U,0x1fU},
  {CrMdssIrqDpAhb,0x034U,IRQ_BIT(2)|IRQ_BIT(5)},
  {CrMdssIrqDpAux,0x00cU,0xfU},
};
static CR_STATUS Final(CR_STATUS S) {
  return S==CR_SUCCESS || CR_ERROR(S) ? S : CR_NOT_READY;
}
static BOOLEAN Overlap(CONST VOID *A,UINTN Na,CONST VOID *B,UINTN Nb) {
  UINTN X=(UINTN)A,Y=(UINTN)B;
  if(X>~(UINTN)0-Na || Y>~(UINTN)0-Nb)return TRUE;
  return X<Y+Nb && Y<X+Na;
}
static CR_STATUS Busy(CR_MDSS_IRQ_SOURCE_OBSERVATION *O,CR_MDSS_IRQ_WINDOW W,
                      UINT32 Offset,UINT32 Value,UINT32 Mask,
                      CR_MDSS_IRQ_SOURCE_OBSERVATION *Out) {
  O->Window=W;O->Offset=Offset;O->Value=Value;O->ForbiddenMask=Mask;*Out=*O;
  return CR_NOT_READY;
}
static CR_STATUS Quiet(CONST CR_MDSS_IRQ_IO *Io,UINT32 AllowedTop0,
                       CONST CR_MDSS_DP_IRQ_CORE *Dp,
                       CR_MDSS_IRQ_SOURCE_OBSERVATION *Out) {
  if(!Io || !Out || !Io->Read32 || !Io->Barrier ||
     (AllowedTop0&~CR_MDSS_IRQ_SUPPORTED) ||
     Overlap(Io,sizeof(*Io),Out,sizeof(*Out)))return CR_INVALID_PARAMETER;
  UINT32 DpMasks=0,DpValue=0,LinkMasks=0,LinkValue=0;
  if(Dp){
    if(Overlap(Dp,sizeof(*Dp),Out,sizeof(*Out))||
       (Dp->Identity&&Overlap(Dp->Identity,sizeof(*Dp->Identity),Out,sizeof(*Out)))||
       (Dp->Link.Identity&&Overlap(Dp->Link.Identity,sizeof(*Dp->Link.Identity),Out,sizeof(*Out))))return CR_INVALID_PARAMETER;
    if(!Dp->Initialized||Dp->Closed||Dp->Faulted||Dp->Io.Context!=Io->Context||
       Dp->Io.Read32!=Io->Read32||Dp->Io.Write32!=Io->Write32||Dp->Io.Barrier!=Io->Barrier)
      return CR_INVALID_PARAMETER;
    if(Dp->Identity){
      if(!Dp->Owner||Dp->Identity->Provider!=Dp||Dp->Identity->Incarnation!=Dp->Incarnation||
         Dp->Identity->Serial!=Dp->Serial)return CR_INVALID_PARAMETER;
      if(Dp->Armed&&Dp->Request)DpMasks=CR_MDSS_DP_AUX_IRQ_MASK;
    }
    if(Dp->Link.Identity){
      if(!Dp->Link.Owner||Dp->Link.Identity->Provider!=Dp||Dp->Link.Identity->Incarnation!=Dp->Incarnation||
         Dp->Link.Identity->Serial!=Dp->Link.Serial)return CR_INVALID_PARAMETER;
      if(Dp->Link.Armed&&Dp->Link.Request){
        if(Dp->Link.Expected!=CR_MDSS_DP_LINK_READY&&Dp->Link.Expected!=CR_MDSS_DP_LINK_IDLE)return CR_INVALID_PARAMETER;
        LinkMasks=Dp->Link.Expected<<2;
      }
    }

  }
  CR_MDSS_IRQ_SOURCE_OBSERVATION O={0};O.Window=CrMdssIrqWindowCount;
  CR_STATUS S=Final(Io->Barrier(Io->Context));if(S!=CR_SUCCESS)return S;
  S=Final(Io->Read32(Io->Context,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,&O.Top0Enabled));
  if(S!=CR_SUCCESS)return S;
  S=Final(Io->Read32(Io->Context,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_STATUS,&O.Top0Pending));
  if(S!=CR_SUCCESS)return S;
  if(O.Top0Enabled&~AllowedTop0)
    return Busy(&O,CrMdssIrqDpu,CR_MDSS_IRQ_TOP0_ENABLE,O.Top0Enabled,~AllowedTop0,Out);
  for(UINT32 I=0;I<sizeof(Sources)/sizeof(Sources[0]);++I){
    UINT32 Value=0;
    S=Final(Io->Read32(Io->Context,Sources[I].Window,Sources[I].Offset,&Value));
    if(S!=CR_SUCCESS)return S;
    UINT32 Forbidden=Sources[I].Mask;
    if(Sources[I].Window==CrMdssIrqDpAhb&&Sources[I].Offset==0x20){
      DpValue=Value;Forbidden&=~DpMasks;
      if(DpMasks&&(Value&DpMasks)!=DpMasks)
        return Busy(&O,CrMdssIrqDpAhb,0x20,Value,DpMasks,Out);
    }
    if(Sources[I].Window==CrMdssIrqDpAhb&&Sources[I].Offset==0x24){
      LinkValue=Value;Forbidden&=~LinkMasks;
      if(LinkMasks&&(Value&LinkMasks)!=LinkMasks)
        return Busy(&O,CrMdssIrqDpAhb,0x24,Value,LinkMasks,Out);
    }
    if(Value&Forbidden)
      return Busy(&O,Sources[I].Window,Sources[I].Offset,Value,Forbidden,Out);
  }
  S=Final(Io->Barrier(Io->Context));if(S!=CR_SUCCESS)return S;
  S=Final(Io->Read32(Io->Context,CrMdssIrqParent,CR_MDSS_IRQ_PARENT_STATUS,&O.ParentStatus));
  if(S!=CR_SUCCESS)return S;
  UINT32 PermittedParent=(O.Top0Enabled&O.Top0Pending&AllowedTop0)?CR_MDSS_IRQ_PARENT_MDP:0;
  if(DpMasks&&(DpValue&CR_MDSS_DP_AUX_IRQ_STATUS))PermittedParent|=CR_MDSS_IRQ_PARENT_DP;
  if(LinkMasks&&(LinkValue&(LinkMasks>>2)))PermittedParent|=CR_MDSS_IRQ_PARENT_DP;
  if(O.ParentStatus&~PermittedParent)
    return Busy(&O,CrMdssIrqParent,CR_MDSS_IRQ_PARENT_STATUS,O.ParentStatus,~PermittedParent,Out);
  *Out=O;return CR_SUCCESS;
}
CR_STATUS CrMdssIrqSourcesQuiet(CONST CR_MDSS_IRQ_IO *Io,UINT32 AllowedTop0,
                                CR_MDSS_IRQ_SOURCE_OBSERVATION *Out) {
  return Quiet(Io,AllowedTop0,NULL,Out);
}
CR_STATUS CrMdssIrqSourcesQuietWithDp(CONST CR_MDSS_IRQ_IO *Io,UINT32 AllowedTop0,
  CONST CR_MDSS_DP_IRQ_CORE *Dp,CR_MDSS_IRQ_SOURCE_OBSERVATION *Out) {
  if(!Dp)return CR_INVALID_PARAMETER;
  return Quiet(Io,AllowedTop0,Dp,Out);
}
