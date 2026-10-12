/* Explicit command-display ownership; native owner supplies OSKAL IRQ/DMA.
 * SPDX-License-Identifier: GPL-2.0-only */
#include "Internal.h"
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
STATIC BOOLEAN MemoryValid(CONST CR_MDSS_CMD_MEMORY *M,UINTN Required,UINT32 Alignment)
{
  return M&&M->Cpu&&M->Ready&&M->Publish&&M->Bytes>=Required&&
    M->Bytes<=(UINTN)-1-(UINTN)M->Cpu&&M->DeviceAddress&&
    !(M->DeviceAddress&(Alignment-1))&&M->DeviceAddress<=CR_MAX_UINT32&&
    M->Bytes-1<=CR_MAX_UINT32-M->DeviceAddress;
}
STATIC BOOLEAN Overlap(UINT64 A,UINT64 Al,UINT64 B,UINT64 Bl)
{return A<B+Bl&&B<A+Al;}
CR_STATUS CrMdssCmdInitProfile(CR_MDSS_CMD *D,CONST CR_MDSS_CMD_IO *Io,
    CONST CR_MDSS_CMD_PROFILE *Profile,CONST CR_MDSS_CMD_MEMORY *Frame,
    CONST CR_MDSS_CMD_MEMORY *Commands)
{
  CR_MDSS_CMD N;CR_STATUS S;
  STATIC CONST UINT64 Physical[CrMdssCmdWindows]={0xae01000,0xae94000,0xae94400,0xaeb0000};
  STATIC CONST UINTN Sizes[CrMdssCmdWindows]={0x83208,0x400,0x7c0,0x2008};
  if(!D||!Io||!Profile||!Io->Ready||!Io->Read32||!Io->Write32||!Io->Fence||
     !Io->DelayUs||!Io->ExternalClocks||!Io->PanelPower||!Io->PanelReset||!Io->Events)
    return CR_INVALID_PARAMETER;
  if(D->Initialized)return CR_ALREADY_STARTED;
  cr_memset(&N,0,sizeof(N));S=CrMdssCmdBuildPlanProfile(Profile,&N.Plan);if(S!=CR_SUCCESS)return S;
  for(UINT32 I=0;I<CrMdssCmdWindows;++I)
    if(Io->Physical[I]!=Physical[I]||Io->Bytes[I]!=Sizes[I])return CR_UNSUPPORTED;
  if(!MemoryValid(Frame,(UINTN)Profile->Config.Stride*Profile->Config.Height,64)||
     !MemoryValid(Commands,256,8))return CR_INVALID_PARAMETER;
  if(Overlap(Frame->DeviceAddress,Frame->Bytes,Commands->DeviceAddress,Commands->Bytes)||
     Overlap((UINTN)Frame->Cpu,Frame->Bytes,(UINTN)Commands->Cpu,Commands->Bytes)||
     Overlap((UINTN)Frame->Cpu,Frame->Bytes,(UINTN)D,sizeof(*D))||
     Overlap((UINTN)Commands->Cpu,Commands->Bytes,(UINTN)D,sizeof(*D)))return CR_INVALID_PARAMETER;
  N.Profile=*Profile;N.Io=*Io;N.Frame=*Frame;N.Commands=*Commands;N.Initialized=TRUE;*D=N;return CR_SUCCESS;
}

CR_STATUS CrMdssCmdInit(CR_MDSS_CMD *D,CONST CR_MDSS_CMD_IO *Io,
    CONST CR_MDSS_CMD_CONFIG *Config,CONST CR_MDSS_CMD_MEMORY *Frame,
    CONST CR_MDSS_CMD_MEMORY *Commands)
{
  CR_MDSS_CMD_PLAN P;CR_STATUS S=CrMdssCmdBuildPlan(Config,&P);if(S!=CR_SUCCESS)return S;
  CR_MDSS_CMD_PROFILE F;CrMdssCmdR66451Profile(&F);F.Config=*Config;
  return CrMdssCmdInitProfile(D,Io,&F,Frame,Commands);
}
CR_STATUS CrMdssCmdStart(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  if(D->Frozen||D->CleanupPending||D->Faulted)return CR_NOT_READY;
  if(D->Active)return CR_ALREADY_STARTED;
  S=CmdPlanCheck(D);if(S!=CR_SUCCESS)return S;
  if(D->Starting){
    if(!D->FirstFrameDone)return CR_NOT_READY;
    if(!D->IrqOwned||!D->ExternalOwned||!D->PanelOwned||!D->PhyTouched||
       !D->HostTouched||!D->PanelTouched||!D->DscTouched||!D->DpuTouched||
       !D->FrameInFlight||!D->AutoRefresh||D->CommandInFlight){S=CR_NOT_READY;goto Failed;}
    /* The frame interrupt is an observation, not a lease. Native providers
     * may have retired between IRQ delivery and GOP publication. Revalidate
     * every resource and both actual DMA mappings before becoming Active. */
    for(UINT32 I=0;I<CrMdssCmdWindows;++I){S=CmdReady(D,(CR_MDSS_CMD_WINDOW)I);if(S!=CR_SUCCESS)goto Failed;}
    S=CmdDmaReady(D,CrMdssCmdDpu);if(S!=CR_SUCCESS)goto Failed;
    S=CmdDmaReady(D,CrMdssCmdHost);if(S!=CR_SUCCESS)goto Failed;
    D->Starting=FALSE;D->Active=TRUE;return CR_SUCCESS;
  }
#define STAGE(x) do{S=Exact(x);if(S!=CR_SUCCESS)goto Failed;}while(0)
  STAGE(CmdDmaReady(D,CrMdssCmdDpu));STAGE(CmdDmaReady(D,CrMdssCmdHost));
  STAGE(D->Io.PanelPower(D->Io.Context,TRUE,&D->PanelOwned));
  if(!D->PanelOwned){S=CR_NOT_READY;goto Failed;}
  STAGE(CmdPhyStart(D));
  STAGE(D->Io.ExternalClocks(D->Io.Context,&D->Plan,TRUE,&D->ExternalOwned));
  if(!D->ExternalOwned){S=CR_NOT_READY;goto Failed;}
  STAGE(CmdHostStart(D));STAGE(CmdPanelOn(D));STAGE(CmdDscStart(D));
  STAGE(D->Frame.Publish(D->Frame.Context,D->Frame.Cpu,
      (UINTN)D->Plan.Config.Stride*D->Plan.Config.Height));
  STAGE(D->Io.Events(D->Io.Context,TRUE,&D->IrqOwned));
  if(!D->IrqOwned){S=CR_NOT_READY;goto Failed;}
  D->Starting=TRUE;
  S=Exact(CmdDpuStart(D));
  if(S==CR_NOT_READY&&D->DpuTouched&&D->FrameInFlight&&D->AutoRefresh)
    return S; /* Accepted kickoff: caller releases lock to deliver frame IRQ. */
  if(S==CR_SUCCESS&&D->FirstFrameDone){D->Starting=FALSE;D->Active=TRUE;return S;}
  if(S==CR_SUCCESS)S=CR_NOT_READY;
Failed:
  D->Starting=FALSE;D->CleanupPending=TRUE;return S;
#undef STAGE
}

CR_STATUS CrMdssCmdHandleIrq(CR_MDSS_CMD *D,UINT32 *Events)
{
  CR_STATUS S;
  if(!D||!D->Initialized||!Events)return CR_INVALID_PARAMETER;
  if((UINTN)Events>(UINTN)-1-sizeof(*Events)||
     (UINTN)D>(UINTN)-1-sizeof(*D)||
     Overlap((UINTN)Events,sizeof(*Events),(UINTN)D,sizeof(*D))||
     Overlap((UINTN)Events,sizeof(*Events),(UINTN)D->Frame.Cpu,D->Frame.Bytes)||
     Overlap((UINTN)Events,sizeof(*Events),(UINTN)D->Commands.Cpu,D->Commands.Bytes))
    return CR_INVALID_PARAMETER;
  *Events=0;
  if(D->Frozen||!D->IrqOwned||(!D->Starting&&!D->Active)||D->CleanupPending)return CR_NOT_READY;
  S=Exact(CmdDpuIrq(D,Events));
  if(S!=CR_SUCCESS){D->Faulted=TRUE;D->Active=FALSE;D->CleanupPending=TRUE;}
  return S;
}

CR_STATUS CrMdssCmdPublish(CR_MDSS_CMD *D,UINTN Offset,UINTN Bytes)
{
  CR_STATUS S;UINTN Limit;
  if(!D||!D->Initialized||!Bytes)return CR_INVALID_PARAMETER;
  Limit=(UINTN)D->Plan.Config.Stride*D->Plan.Config.Height;
  if(Offset>=Limit||Bytes>Limit-Offset)return CR_BAD_BUFFER_SIZE;
  if(D->Frozen||!D->Active||D->CleanupPending||D->Faulted)return CR_NOT_READY;
  S=CmdDmaReady(D,CrMdssCmdDpu);if(S!=CR_SUCCESS)return S;
  /* Hardware auto-refresh consumes this retained frame. No CPU redraw loop. */
  return Exact(D->Frame.Publish(D->Frame.Context,(UINT8*)D->Frame.Cpu+Offset,Bytes));
}

CR_STATUS CrMdssCmdStop(CR_MDSS_CMD *D)
{
  CR_STATUS S;
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  if(D->Frozen)return CR_NOT_READY;
  D->Active=FALSE;D->Starting=FALSE;D->CleanupPending=TRUE;
#define STOP(x) do{S=Exact(x);if(S!=CR_SUCCESS)return S;}while(0)
  if(D->IrqOwned){STOP(D->Io.Events(D->Io.Context,FALSE,&D->IrqOwned));
    if(D->IrqOwned)return CR_NOT_READY;}
  STOP(CmdDpuStop(D));STOP(CmdPanelOff(D));STOP(CmdHostStop(D));STOP(CmdDscStop(D));
  if(D->ExternalOwned){
    STOP(D->Io.ExternalClocks(D->Io.Context,&D->Plan,FALSE,&D->ExternalOwned));
    if(D->ExternalOwned)return CR_NOT_READY;
  }
  STOP(CmdPhyStop(D));
  if(D->PanelOwned){STOP(D->Io.PanelPower(D->Io.Context,FALSE,&D->PanelOwned));
    if(D->PanelOwned)return CR_NOT_READY;}
  D->FirstFrameDone=FALSE;D->Faulted=FALSE;D->CleanupPending=FALSE;
  return CR_SUCCESS;
#undef STOP
}
CR_STATUS CrMdssCmdUninit(CR_MDSS_CMD *D)
{
  if(!D||!D->Initialized)return CR_INVALID_PARAMETER;
  if(D->Frozen||D->Starting||D->Active||D->CleanupPending||D->ExternalOwned||D->PanelOwned||
     D->IrqOwned||D->PhyTouched||D->HostTouched||D->PanelTouched||D->DscTouched||
     D->DpuTouched||D->CommandInFlight||D->FrameInFlight||D->AutoRefresh||
     D->DpuForceTouched||D->DpuHaltTouched)return CR_NOT_READY;
  cr_memset(D,0,sizeof(*D));return CR_SUCCESS;
}
