/* Physical retirement of the admitted SM8250 boot command display.
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 * Register order is from cad09e0 sde_hw_{ctl,vbif,intf}.c,
 * dsi_ctrl_hw_cmn.c, dsi_phy_hw_v4_0.c and dsi_pll_7nm.c.
 * No SMMU mutation, clock reparent, or fabricated cold-pipeline state. */
#include "Internal.h"
#include <oskal/cr_mutex.h>
#define DPU CrMdssBootDpu
#define VBIF CrMdssBootVbif
#define HOST CrMdssBootHost0
#define PHY CrMdssBootPhy0
#define CTL 0x1000U
#define INTF 0x6a800U
#define PP 0x70000U
#define DSC 0x80000U
#define REGDMA_XIN BOOTBIT(7)
#define REGDMA_FORCE BOOTBIT(20)
#define TRY(E) do { CR_STATUS S=(E);if(S!=CR_SUCCESS)return S; } while(0)
static CR_STATUS Exact(CR_STATUS S)
{return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}

static CR_STATUS Identity(CR_MDSS_BOOT8250 *P)
{
  if(!P||!P->Initialized||!P->Admitted)return CR_NOT_READY;
  const CR_MDSS_BOOT_OBSERVATION *O=&P->Before;
  if(O->Revision!=CR_MDSS_BOOT8250_REVISION ||
     (O->SourceIndex!=0&&O->SourceIndex!=4) ||
     O->Xin!=(O->SourceIndex==0?0U:1U) ||
     O->SourceOffset!=(O->SourceIndex==0?0x4000U:0x24000U) ||
     O->ForceOffset!=0x2ac || O->ForceMask!=BOOTBIT(O->SourceIndex==0?0:8) ||
     O->Width!=1080 || O->Height!=2340 || !O->FrameDevice || !O->FrameBytes)
    return CR_COMPROMISED_DATA;
  return CR_SUCCESS;
}

typedef struct {UINT32 Offset,Mask;} FIELD;
static const FIELD PhyOffFields[]={
  {0x0a4,0x60},{0x014,0x20},{0x03c,1},{0x01c,0xff},
  {0x528,0xff},{0x024,0x20},{0x214,0xff},
  {0x024,0x1f},{0x0a0,0xff},{0x024,0xff}
};

static CR_STATUS Zero(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_WINDOW W,UINT32 O,UINT32 Mask)
{UINT32 V;TRY(BootRead(P,W,O,&V));return V&Mask?CR_NOT_READY:CR_SUCCESS;}

static CR_STATUS MaskIrq(CR_MDSS_BOOT8250 *P)
{
  for(UINT32 I=0;I<CR_MDSS_BOOT8250_IRQ_BANKS;++I)
    if(BootIrqAllowed[I])
      TRY(BootUpdate(P,DPU,BootIrqOffsets[I][0],BootIrqAllowed[I],0));
  /* DSI INT_CTRL interleaves W1C status and enable. Zero masks all enables
   * without acknowledging a status bit; final idle precedes status clear. */
  TRY(BootWrite(P,HOST,0x110,0));TRY(BootFence(P,HOST));
  return Zero(P,HOST,0x110,0xaaaaaaaaU);
}
static CR_STATUS AckIrq(CR_MDSS_BOOT8250 *P)
{
  for(UINT32 I=0;I<CR_MDSS_BOOT8250_IRQ_BANKS;++I){
    UINT32 Pending;TRY(BootRead(P,DPU,BootIrqOffsets[I][1],&Pending));
    Pending&=BootIrqAllowed[I];
    if(!Pending)continue;
    TRY(BootWrite(P,DPU,BootIrqOffsets[I][2],Pending));TRY(BootFence(P,DPU));
    TRY(Zero(P,DPU,BootIrqOffsets[I][1],Pending));
  }
  return CR_SUCCESS;
}
static CR_STATUS FreshHalt(CR_MDSS_BOOT8250 *P,UINT32 Mask,BOOLEAN RegDma)
{
  UINT32 Offset=RegDma?0x2bc:P->Before.ForceOffset;
  UINT32 Force=RegDma?REGDMA_FORCE:P->Before.ForceMask;
  BOOLEAN *Touched=RegDma?&P->RegDmaForceTouched:&P->ForceTouched;
  UINT32 *Original=RegDma?&P->RegDmaForceOriginal:&P->ForceOriginal;
  UINT32 V;TRY(BootRead(P,DPU,Offset,&V));
  if(!*Touched){*Original=V;*Touched=TRUE;}
  TRY(BootChecked(P,DPU,Offset,V|Force,Force));
  TRY(BootUpdate(P,VBIF,0x200,Mask,0));
  TRY(BootPoll(P,VBIF,0x204,Mask,0,2000));
  TRY(BootUpdate(P,VBIF,0x200,Mask,Mask));
  /* Kona ACK is the low field. A retained high cannot satisfy the first
   * low edge; a delayed high is required only after our new request. */
  return BootPoll(P,VBIF,0x204,Mask,Mask,2000);
}
static CR_STATUS RestoreForce(CR_MDSS_BOOT8250 *P)
{
  if(P->ForceTouched){
    TRY(BootUpdate(P,DPU,P->Before.ForceOffset,P->Before.ForceMask,P->ForceOriginal));
    P->ForceTouched=FALSE;
  }
  if(P->RegDmaForceTouched){
    TRY(BootUpdate(P,DPU,0x2bc,REGDMA_FORCE,P->RegDmaForceOriginal));
    P->RegDmaForceTouched=FALSE;
  }
  return CR_SUCCESS;
}
static CR_STATUS Drain(CR_MDSS_BOOT8250 *P)
{
  /* Software sealing may commit even when its callback fails. Abort can no
   * longer withdraw the inherited access lease after this point. */
  P->Touched=TRUE;
  TRY(BootReady(P,DPU));TRY(Exact(P->Io.DrainWriters(P->Io.Context)));
  P->WritersDrained=TRUE;
  CR_MDSS_BOOT_OBSERVATION Now;
  TRY(BootObserve(P,&Now));TRY(BootAdmit(&Now));
  return BootSameRoute(&P->Before,&Now);
}
static CR_STATUS AutoStatus(CR_MDSS_BOOT8250 *P,UINT32 *Status)
{
  TRY(BootChecked(P,DPU,0x418,0x405,0xffffffffU));
  TRY(BootChecked(P,DPU,0x348,0x7001,0xffffffffU));
  TRY(BootRead(P,DPU,0x34c,Status));
  return BootChecked(P,DPU,0x418,0,0xffffffffU);
}
static CR_STATUS StopTe(CR_MDSS_BOOT8250 *P)
{
  TRY(BootUpdate(P,DPU,INTF+0x288,BOOTBIT(20),0));
  TRY(BootUpdate(P,DPU,INTF+0x2b4,BOOTBIT(31),0));
  /* Android disable_seq1 waits before the first observation. Tearcheck must
   * remain enabled here: disabling it can turn an ongoing count into zero. */
  for(UINT32 I=0;I<17;++I){
    TRY(BootDelay(P,DPU,2000));
    UINT32 V;TRY(BootRead(P,DPU,INTF+0x2b0,&V));V&=0xffff;
    if(!V||V>=P->Before.Height)goto LineIdle;
  }
  return CR_TIMEOUT;
LineIdle:
  /* The line count alone does not exclude queued autorefresh. Use the
   * original mdptop debug selector (INTF1 => block64/test2), retaining both
   * selectors across partial failures. Never interpret a stale unrelated
   * debug value after a dropped selector write as idle. */
  if(!P->DebugTouched){
    UINT32 Periph,Dspp;
    TRY(BootRead(P,DPU,0x418,&Periph));TRY(BootRead(P,DPU,0x348,&Dspp));
    P->DebugPeriphOriginal=Periph;P->DebugDsppOriginal=Dspp;
    P->DebugTouched=TRUE;
  }
  UINT32 Status;TRY(AutoStatus(P,&Status));
  for(UINT32 Trial=0;Status&BOOTBIT(7);++Trial){
    if(Trial==40)return CR_TIMEOUT;
    if(!Trial)TRY(BootUpdate(P,DPU,INTF+0x2b4,BOOTBIT(31),0));
    TRY(BootDelay(P,DPU,25000));
    TRY(AutoStatus(P,&Status));
    /* Android check_and_reset_tearcheck replaces only the low write-count
     * field with START_POS so a pending trigger can leave its window. */
    UINT32 Start,Count,ReadCount;
    TRY(BootRead(P,DPU,INTF+0x298,&ReadCount));
    TRY(BootRead(P,DPU,INTF+0x2a0,&Start));
    TRY(BootRead(P,DPU,INTF+0x290,&Count));
    if(Start&0xffff0000U)return CR_UNSUPPORTED;
    TRY(BootChecked(P,DPU,INTF+0x290,(Count&0xffff0000U)|Start,0xffffffffU));
  }
  TRY(BootChecked(P,DPU,0x418,P->DebugPeriphOriginal,0xffffffffU));
  TRY(BootChecked(P,DPU,0x348,P->DebugDsppOriginal,0xffffffffU));
  P->DebugTouched=FALSE;
  return BootChecked(P,DPU,INTF+0x284,0,1);
}
static CR_STATUS RemoveRoute(CR_MDSS_BOOT8250 *P)
{
  static const UINT32 LayerOffsets[]={0,0x40,0x70,0xa0};
  for(UINT32 I=0;I<4;++I)TRY(BootChecked(P,DPU,CTL+LayerOffsets[I],0,0xffffffffU));
  TRY(BootChecked(P,DPU,CTL+0xe8,0,1));
  TRY(BootChecked(P,DPU,CTL+0xf4,0,2));
  TRY(BootChecked(P,DPU,INTF,0,1));
  TRY(BootWrite(P,DPU,CTL+0x30,1));TRY(BootFence(P,DPU));
  TRY(BootPoll(P,DPU,CTL+0x30,1,0,2000));
  /* Reset completion and pending flush retirement are separate evidence. */
  return BootPoll(P,DPU,CTL+0x18,P->Before.FlushMask,0,2000);
}
static CR_STATUS HostOff(CR_MDSS_BOOT8250 *P)
{
  TRY(BootReady(P,HOST));TRY(Exact(P->Io.PanelResetOff(P->Io.Context)));
  TRY(BootWrite(P,HOST,0x11c,0x23fU|BOOTBIT(8)|BOOTBIT(9)|BOOTBIT(11)|BOOTBIT(21)));
  TRY(BootFence(P,HOST));TRY(BootPoll(P,HOST,8,6,0,50000));
  TRY(BootUpdate(P,HOST,4,7,0));TRY(BootPoll(P,HOST,4,7,0,1000));
  TRY(BootPoll(P,HOST,8,6,0,5000));
  TRY(BootWrite(P,HOST,0x110,0));TRY(BootWrite(P,HOST,0x11c,0));
  return BootFence(P,HOST);
}
static CR_STATUS DscOff(CR_MDSS_BOOT8250 *P)
{
  TRY(BootUpdate(P,DPU,PP+0xa0,1,0));
  TRY(BootUpdate(P,DPU,DSC+0x1800,0xf,0xf));
  return BootChecked(P,DPU,DSC,0,0xffffffffU);
}
static CR_STATUS PhyOff(CR_MDSS_BOOT8250 *P)
{
  for(UINT32 I=0;I<sizeof(PhyOffFields)/sizeof(PhyOffFields[0]);++I){
    const FIELD *F=&PhyOffFields[I];
    TRY(BootUpdate(P,PHY,F->Offset,F->Mask,0));
    TRY(BootPoll(P,PHY,F->Offset,F->Mask,0,1000));
    if(F->Offset==0x024&&F->Mask==0x20)TRY(BootDelay(P,PHY,1));
  }
  return BootFence(P,PHY);
}

static CR_STATUS CheckFence(CR_MDSS_BOOT8250 *P)
{
  if(P->DebugTouched)return CR_NOT_READY;
  UINT32 V,Mask=BOOTBIT(P->Before.Xin)|REGDMA_XIN;
  /* Every live path removed before the physical halt must remain removed.
   * This is a read-only check and grants no SMMU transfer by itself. */
  for(UINT32 C=0;C<6;++C){
    UINT32 B=CTL+C*0x200;
    static const UINT32 Routes[]={0xe4,0xe8,0xec,0xf0,0xf4,0xf8};
    for(UINT32 R=0;R<sizeof(Routes)/sizeof(Routes[0]);++R)
      TRY(Zero(P,DPU,B+Routes[R],0xffffffffU));
  }
  TRY(Zero(P,DPU,CTL+0x30,1));TRY(Zero(P,DPU,CTL+0x18,P->Before.FlushMask));
  TRY(Zero(P,DPU,INTF+0x288,BOOTBIT(20)));
  TRY(Zero(P,DPU,INTF+0x284,1));TRY(Zero(P,DPU,INTF+0x2b4,BOOTBIT(31)));
  for(UINT32 I=0;I<4;++I){
    TRY(Zero(P,DPU,0x6a000+0x800*I,1));
    TRY(Zero(P,DPU,0x6a26c+0x800*I,0xffffffffU));
  }
  TRY(BootRead(P,VBIF,0x200,&V));if((V&Mask)!=Mask)return CR_NOT_READY;
  TRY(BootRead(P,VBIF,0x204,&V));if((V&Mask)!=Mask)return CR_NOT_READY;
  for(UINT32 I=0;I<CR_MDSS_BOOT8250_IRQ_BANKS;++I)
    TRY(Zero(P,DPU,BootIrqOffsets[I][0],0xffffffffU));
  TRY(Zero(P,CrMdssBootRegDma,4,1));
  for(UINT32 I=0;I<2;++I){
    CR_MDSS_BOOT_WINDOW H=I?CrMdssBootHost1:HOST;
    TRY(Zero(P,H,4,7));TRY(Zero(P,H,8,6));TRY(Zero(P,H,0x110,0xaaaaaaaaU));
  }
  TRY(Zero(P,DPU,PP+0xa0,1));TRY(BootRead(P,DPU,DSC+0x1800,&V));
  if((V&0xf)!=0xf)return CR_NOT_READY;
  for(UINT32 I=0;I<sizeof(PhyOffFields)/sizeof(PhyOffFields[0]);++I)
    TRY(Zero(P,PHY,PhyOffFields[I].Offset,PhyOffFields[I].Mask));
  TRY(Zero(P,CrMdssBootPhy1,0x03c,1));
  TRY(Zero(P,CrMdssBootPhy1,0x024,0x7f));
  TRY(Zero(P,CrMdssBootPhy1,0x01c,0xff));
  TRY(Zero(P,CrMdssBootPhy1,0x528,0xff));
  TRY(BootReady(P,PHY));TRY(Exact(P->Io.PanelResetReadyOff(P->Io.Context)));
  TRY(BootReady(P,PHY));return Exact(P->Io.ExternalClocksReadyOff(P->Io.Context));
}

CR_STATUS CrMdssBoot8250CheckFence(CR_MDSS_BOOT8250 *P)
{
  if(CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(!P||!P->Initialized||!P->Admitted||!P->WritersDrained||!P->Touched||P->Stage<8)
    return CR_NOT_READY;
  TRY(Identity(P));
  CR_STATUS S=CheckFence(P);
  if(S!=CR_SUCCESS)P->Retired=FALSE;
  P->LastStatus=S;return S;
}

static CR_STATUS Retire(CR_MDSS_BOOT8250 *P)
{
  while(P->Stage<9){
    switch(P->Stage){
    case 0: TRY(Drain(P));break;
    case 1:
      TRY(MaskIrq(P));
      /* REGDMA was admitted only when disabled with empty queues. Still
       * fence its real XIN before any staged route can be changed. */
      TRY(FreshHalt(P,REGDMA_XIN,TRUE));break;
    case 2: TRY(StopTe(P));TRY(RemoveRoute(P));break;
    case 3: TRY(FreshHalt(P,BOOTBIT(P->Before.Xin),FALSE));TRY(AckIrq(P));break;
    case 4: TRY(HostOff(P));break;
    case 5: TRY(DscOff(P));break;
    case 6:
      TRY(BootReady(P,PHY));TRY(Exact(P->Io.ExternalClocksOff(P->Io.Context)));break;
    case 7: TRY(PhyOff(P));break;
    case 8: TRY(RestoreForce(P));TRY(CheckFence(P));break;
    default:return CR_COMPROMISED_DATA;
    }
    ++P->Stage;
  }
  TRY(CheckFence(P));P->Retired=TRUE;return CR_SUCCESS;
}
CR_STATUS CrMdssBoot8250Retire(CR_MDSS_BOOT8250 *P)
{
  if(CrMutexCheckLevel()!=CR_SUCCESS)return CR_INVALID_PARAMETER;
  if(!P||!P->Initialized)return CR_INVALID_PARAMETER;
  if(P->Retired)return CrMdssBoot8250CheckFence(P);
  if(!P->Admitted){
    CR_STATUS S=CrMdssBoot8250Observe(P);if(S!=CR_SUCCESS)return S;
  }
  TRY(Identity(P));
  CR_STATUS S=Retire(P);P->LastStatus=S;return S;
}
