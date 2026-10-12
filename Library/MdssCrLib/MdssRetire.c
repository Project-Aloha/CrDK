/* Whole nonsecure MDSS retirement under an exclusive native owner.
 * Public Linux DPU6/7 catalogs, dpu_hw_{ctl,vbif}.c, DSI host reset and
 * Android DSI PHY4.1/4.2 shutdown. No SMMU or supply mutation.
 * SPDX-License-Identifier: GPL-2.0-only */
#include <Library/MdssRetireCrLib.h>
#include <oskal/cr_string.h>
#define TRY(E) do {CR_STATUS S=(E);if(S!=CR_SUCCESS)return S;} while(0)
#define XINS 0x33f3U /* VIG0..3, DMA0..3, WB2, register DMA */
STATIC CONST UINT32 ForceOffsets[]={0x2ac,0x2b4,0x2bc,0x2c4};
STATIC CONST UINT32 ForceMasks[]={0x101,0x101,0x110101,0x101};
STATIC CR_STATUS Exact(CR_STATUS S){return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;}
STATIC CR_STATUS Read(CONST CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 *V){
  TRY(Exact(R->Io.Ready(R->Io.Context,W)));return Exact(R->Io.Read32(R->Io.Context,W,O,V));
}
STATIC CR_STATUS Write(CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 V){
  TRY(Exact(R->Io.Ready(R->Io.Context,W)));return Exact(R->Io.Write32(R->Io.Context,W,O,V));
}
STATIC CR_STATUS Fence(CR_MDSS_RETIRE *R){return Exact(R->Io.Fence(R->Io.Context));}
STATIC CR_STATUS Delay(CR_MDSS_RETIRE *R,UINT32 Us){return Exact(R->Io.DelayUs(R->Io.Context,Us));}
STATIC CR_STATUS Zero(CONST CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 Mask){
  UINT32 V;TRY(Read(R,W,O,&V));return V&Mask?CR_NOT_READY:CR_SUCCESS;
}
STATIC CR_STATUS ReadbackFailure(CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W,
  UINT32 O,UINT32 Mask,UINT32 Expected,UINT32 Actual,CR_STATUS Status){
  R->ReadbackFailed=TRUE;R->FailureWindow=W;R->FailureOffset=O;
  R->FailureMask=Mask;R->FailureExpected=Expected&Mask;R->FailureActual=Actual;
  return Status;
}
STATIC CR_STATUS Update(CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 Mask,UINT32 Value){
  UINT32 V;TRY(Read(R,W,O,&V));TRY(Write(R,W,O,(V&~Mask)|(Value&Mask)));TRY(Fence(R));
  TRY(Read(R,W,O,&V));return (V&Mask)==(Value&Mask)?CR_SUCCESS:
    ReadbackFailure(R,W,O,Mask,Value,V,CR_DEVICE_ERROR);
}
STATIC CR_STATUS Poll(CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W,UINT32 O,UINT32 Mask,UINT32 Value){
  for(UINT32 T=0;;T+=50){UINT32 V;TRY(Read(R,W,O,&V));if((V&Mask)==Value)return CR_SUCCESS;
    if(T==2000)return ReadbackFailure(R,W,O,Mask,Value,V,CR_TIMEOUT);TRY(Delay(R,50));}
}
STATIC CR_STATUS RequestClocks(CR_MDSS_RETIRE *R,UINT32 I){
  UINT32 V;TRY(Read(R,CrMdssRetireDpu,ForceOffsets[I],&V));
  TRY(Write(R,CrMdssRetireDpu,ForceOffsets[I],V|ForceMasks[I]));TRY(Fence(R));
  TRY(Read(R,CrMdssRetireDpu,ForceOffsets[I],&R->ForceReadback[I]));
  R->ForceMissing[I]=ForceMasks[I]&~R->ForceReadback[I];
  /* Public dpu_hw_clk_force_ctrl / dpu_vbif: request the clock, then
     wait for the XIN halt ACK. A force bit need not reflect a writable
     latch. Keep the readback for diagnosis; the full fresh halt below
     remains mandatory before PHY shutdown or any SMMU retirement. */
  return CR_SUCCESS;
}
STATIC CR_STATUS Version(CONST CR_MDSS_RETIRE *R){
  UINT32 V;TRY(Read(R,CrMdssRetireDpu,0,&V));
  return (V>>28)==R->Hardware->DpuMajor&&((V>>16)&0xfff)==R->Hardware->DpuMinor?CR_SUCCESS:CR_UNSUPPORTED;
}
CR_STATUS CrMdssRetireInit(CR_MDSS_RETIRE *R,CONST CR_MDSS_RETIRE_IO *Io){
  if(!R||!Io||!Io->Context||!Io->Ready||!Io->SealWriters||!Io->Read32||!Io->Write32||
    !Io->Fence||!Io->DelayUs||!Io->ResetCore||!Io->CoreResetReleased||
    !Io->ExternalClocksOff||!Io->ExternalClocksReadyOff)return CR_INVALID_PARAMETER;
  if(R->Initialized)return CR_ALREADY_STARTED;
  CONST CR_MDSS_HW_PROFILE *H=CrMdssGetHardwareProfile(Io->Soc);
  if(!H||(Io->Soc!=CR_MDSS_SOC_SM8250&&Io->Soc!=CR_MDSS_SOC_SM8350))return CR_UNSUPPORTED;
  cr_memset(R,0,sizeof(*R));R->Io=*Io;R->Hardware=H;R->Initialized=TRUE;return CR_SUCCESS;
}
STATIC CR_STATUS HostOff(CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W){
  TRY(Update(R,W,4,7,0));TRY(Write(R,W,0x110,0));TRY(Fence(R));
  TRY(Write(R,W,0x118,1));TRY(Fence(R));TRY(Delay(R,20000));
  TRY(Write(R,W,0x118,0));TRY(Fence(R));
  TRY(Poll(R,W,4,7,0));return Poll(R,W,8,6,0);
}
STATIC CR_STATUS PhyOff(CR_MDSS_RETIRE *R,CR_MDSS_RETIRE_WINDOW W){
  STATIC CONST UINT32 Fields[][2]={{0xa4,0x60},{0x14,0x20},{0x3c,1},{0x1c,0xff},
    {0x528,0xff},{0x24,0x20},{0x214,0xff},{0x24,0x1f},{0xa0,0xff},{0x24,0xff}};
  for(UINT32 I=0;I<sizeof(Fields)/sizeof(Fields[0]);++I){
    TRY(Update(R,W,Fields[I][0],Fields[I][1],0));
    TRY(Poll(R,W,Fields[I][0],Fields[I][1],0));
    if(Fields[I][0]==0x24&&Fields[I][1]==0x20)TRY(Delay(R,1));
  }
  return CR_SUCCESS;
}
STATIC CR_STATUS PipelinesStopped(CONST CR_MDSS_RETIRE *R){
  TRY(Version(R));TRY(Exact(R->Io.CoreResetReleased(R->Io.Context)));
  for(UINT32 I=0;I<2;++I){
    CR_MDSS_RETIRE_WINDOW W=I?CrMdssRetireDsi1:CrMdssRetireDsi0;
    TRY(Zero(R,W,4,7));TRY(Zero(R,W,8,6));TRY(Zero(R,W,0x110,0xaaaaaaaa));
    W=I?CrMdssRetirePhy1:CrMdssRetirePhy0;
    TRY(Zero(R,W,0x3c,1));TRY(Zero(R,W,0x24,0x7f));TRY(Zero(R,W,0x14,0x20));
    TRY(Zero(R,W,0x1c,0xff));TRY(Zero(R,W,0x214,0xff));
  }
  UINT32 Stride=R->Io.Soc==CR_MDSS_SOC_SM8250?0x200:0x1000;
  for(UINT32 I=0;I<6;++I){UINT32 C=R->Hardware->Ctl+I*Stride;
    TRY(Zero(R,CrMdssRetireDpu,C+0x30,1));
    TRY(Zero(R,CrMdssRetireDpu,C+0x1c,1));
    TRY(Zero(R,CrMdssRetireDpu,C+0x18,0xffffffff));}
  UINT32 IntfStride=R->Io.Soc==CR_MDSS_SOC_SM8250?0x800:0x1000;
  UINT32 First=R->Hardware->Intf[0]-IntfStride;
  for(UINT32 I=0;I<4;++I){UINT32 B=First+I*IntfStride;
    TRY(Zero(R,CrMdssRetireDpu,B,1));
    /* Only DSI interfaces implement these registers; DP ends at 0x280. */
    if(I==1||I==2){
      TRY(Zero(R,CrMdssRetireDpu,B+0x84,1));
      TRY(Zero(R,CrMdssRetireDpu,B+0x284,1));
      TRY(Zero(R,CrMdssRetireDpu,B+0x2b4,0x80000000));}}
  return Exact(R->Io.ExternalClocksReadyOff(R->Io.Context));
}
CR_STATUS CrMdssRetireCheck(CONST CR_MDSS_RETIRE *R){
  if(!R||!R->Initialized)return CR_INVALID_PARAMETER;
  if(!R->WritersSealed||!R->Retired||R->Resumed||R->ResumeStage)return CR_NOT_READY;
  UINT32 V;TRY(Read(R,CrMdssRetireVbif,0x200,&V));if((V&XINS)!=XINS)return CR_NOT_READY;
  TRY(Read(R,CrMdssRetireVbif,0x204,&V));if((V&XINS)!=XINS)return CR_NOT_READY;
  return PipelinesStopped(R);
}
CR_STATUS CrMdssRetireStart(CR_MDSS_RETIRE *R){
  if(!R||!R->Initialized)return CR_INVALID_PARAMETER;
  R->ReadbackFailed=FALSE;
  if(R->Resumed||R->ResumeStage)return CR_BUSY;
  if(R->Retired)return CrMdssRetireCheck(R);
  /* Public msm_mdss_probe resets before functional-clock setup. Never read
     DPU or change an inherited functional source before the early BCR; a
     failed assertion may have taken effect, so retry that owner first. */
  if(R->Stage==0){R->Touched=TRUE;TRY(Exact(R->Io.SealWriters(R->Io.Context)));
    R->WritersSealed=TRUE;R->Stage=1;}
  if(R->Stage==1){TRY(Exact(R->Io.ResetCore(R->Io.Context,TRUE)));TRY(Delay(R,20000));
    TRY(Exact(R->Io.ResetCore(R->Io.Context,FALSE)));TRY(Delay(R,50));
    TRY(Exact(R->Io.CoreResetReleased(R->Io.Context)));TRY(Version(R));R->Stage=2;}
  if(R->Stage==2){TRY(HostOff(R,CrMdssRetireDsi0));R->Stage=3;}
  if(R->Stage==3){TRY(HostOff(R,CrMdssRetireDsi1));R->Stage=4;}
  if(R->Stage==4){
    if(!R->ForceSaved){for(UINT32 I=0;I<4;++I)TRY(Read(R,CrMdssRetireDpu,ForceOffsets[I],&R->OriginalForce[I]));R->ForceSaved=TRUE;}
    for(UINT32 I=0;I<4;++I)TRY(RequestClocks(R,I));
    TRY(Update(R,CrMdssRetireVbif,0x200,XINS,0));TRY(Poll(R,CrMdssRetireVbif,0x204,XINS,0));
    TRY(Update(R,CrMdssRetireVbif,0x200,XINS,XINS));TRY(Poll(R,CrMdssRetireVbif,0x204,XINS,XINS));R->Stage=5;
  }
  if(R->Stage==5){TRY(PhyOff(R,CrMdssRetirePhy0));R->Stage=6;}
  if(R->Stage==6){TRY(PhyOff(R,CrMdssRetirePhy1));R->Stage=7;}
  if(R->Stage==7){TRY(Exact(R->Io.ExternalClocksOff(R->Io.Context)));R->Stage=8;}
  R->Retired=TRUE;CR_STATUS S=CrMdssRetireCheck(R);
  if(S!=CR_SUCCESS)R->Retired=FALSE;return S;
}
CR_STATUS CrMdssRetireResume(CR_MDSS_RETIRE *R){
  if(!R||!R->Initialized)return CR_INVALID_PARAMETER;
  R->ReadbackFailed=FALSE;
  if(R->Resumed)return CR_SUCCESS;
  if(!R->ResumeStage){
    TRY(CrMdssRetireCheck(R));
    R->ResumeStage=1;R->Retired=FALSE;
  }
  /* This proof deliberately excludes the halt condition being withdrawn.
     Writers remain sealed and every host/PHY/CTL/INTF remains stopped. */
  TRY(PipelinesStopped(R));
  if(R->ResumeStage==1){
    TRY(Update(R,CrMdssRetireVbif,0x200,XINS,0));
    TRY(Poll(R,CrMdssRetireVbif,0x204,XINS,0));R->ResumeStage=2;
  }
  for(;R->ResumeForce<4;++R->ResumeForce){
    UINT32 I=R->ResumeForce;
    TRY(Update(R,CrMdssRetireDpu,ForceOffsets[I],ForceMasks[I],R->OriginalForce[I]));
  }
  R->ForceSaved=FALSE;R->Resumed=TRUE;return CR_SUCCESS;
}
