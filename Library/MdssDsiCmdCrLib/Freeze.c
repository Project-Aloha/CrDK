/* Native EBS DMA fence with permanent resource/IRQ lifetime retention.
 * Uses the qualified command Stop ordering and DSI2.4 software reset.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
#define TRY(X) do{CR_STATUS S_=(X);if(S_!=CR_SUCCESS)return CR_ERROR(S_)?S_:CR_NOT_READY;}while(0)
STATIC CR_STATUS Observe(CR_MDSS_CMD *D,CR_MDSS_CMD_WINDOW W,UINT32 O,UINT32 V,UINT32 M){
  UINT32 A;TRY(CmdRead(D,W,O,&A));return ((A^V)&M)?CR_NOT_READY:CR_SUCCESS;
}
STATIC CR_STATUS Check(CR_MDSS_CMD *D){
  if(!D->IrqFrozen||D->DpuTouched||D->FrameInFlight||D->AutoRefresh||
     D->HostTouched||D->CommandInFlight)return CR_NOT_READY;
  TRY(CmdPlanCheck(D));
  /* Explicit same-owner route, kickoff/flush, TE/autorefresh and host idle
   * observations are repeated before the producer can revoke command pages. */
  TRY(Observe(D,CrMdssCmdDpu,0x1000,0,0xffffffffU));
  TRY(Observe(D,CrMdssCmdDpu,0x1030,0,1));
  TRY(Observe(D,CrMdssCmdDpu,0x1018,0,(1U<<11)|(1U<<6)|(1U<<17)|(1U<<22)|(1U<<31)));
  TRY(Observe(D,CrMdssCmdDpu,0x6aa84,0,1));
  TRY(Observe(D,CrMdssCmdDpu,0x6aa88,0,1U<<20));
  TRY(Observe(D,CrMdssCmdDpu,0x6aab4,0,1U<<31));
  TRY(Observe(D,CrMdssCmdVbif,0x200,2,2));TRY(Observe(D,CrMdssCmdVbif,0x204,2,2));
  TRY(Observe(D,CrMdssCmdHost,4,0,7));TRY(Observe(D,CrMdssCmdHost,8,0,6));
  TRY(Observe(D,CrMdssCmdHost,0x110,0,0xaaaaaaaaU));
  TRY(Observe(D,CrMdssCmdHost,0x118,0,1));
  TRY(CmdDmaReady(D,CrMdssCmdDpu));TRY(CmdDmaReady(D,CrMdssCmdHost));
  return CR_SUCCESS;
}
CR_STATUS CrMdssCmdFreeze(CR_MDSS_CMD *D){
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  if(!D->Io.FreezeEvents)return CR_UNSUPPORTED;
  TRY(CmdPlanCheck(D));D->Frozen=TRUE;D->Active=FALSE;D->Starting=FALSE;D->FreezeComplete=FALSE;
  /* Recheck the live mask/rundown witness on every invocation. Ownership is
   * retained in IrqOwned, never falsified to bypass an IRQ lifetime rule. */
  BOOLEAN Sealed=FALSE;TRY(D->Io.FreezeEvents(D->Io.Context,&Sealed));
  if(!Sealed)return CR_NOT_READY;D->IrqFrozen=TRUE;
  if(D->FreezeStage<1){
    /* Even a pre-start owner needs an actual physical route fence, not an
     * inference from software untouched flags. The source is exclusive. */
    D->DpuTouched=TRUE;TRY(CmdDpuStop(D));D->FreezeStage=1;
  }
  if(D->FreezeStage<2){
    D->HostTouched=TRUE;TRY(CmdHostStop(D));
    TRY(CmdChecked(D,CrMdssCmdHost,0x11c,0x23fU|(1U<<8)|(1U<<9)|(1U<<11)|(1U<<21),0xffffffffU));
    TRY(CmdChecked(D,CrMdssCmdHost,0x118,1,1));TRY(CmdFence(D,CrMdssCmdHost));
    TRY(CmdDelay(D,CrMdssCmdHost,1));
    TRY(CmdChecked(D,CrMdssCmdHost,0x118,0,1));TRY(CmdFence(D,CrMdssCmdHost));
    TRY(CmdChecked(D,CrMdssCmdHost,4,0,7));
    TRY(CmdChecked(D,CrMdssCmdHost,0x110,0,0xaaaaaaaaU));
    TRY(CmdChecked(D,CrMdssCmdHost,0x11c,0,0xffffffffU));
    D->HostTouched=FALSE;D->CommandInFlight=FALSE;D->FreezeStage=2;
  }
  TRY(Check(D));D->FreezeComplete=TRUE;return CR_SUCCESS;
}
