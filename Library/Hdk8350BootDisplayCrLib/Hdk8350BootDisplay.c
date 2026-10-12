/* Read-only SM8350 HDK preserved-video inventory and admission.
 * SPDX-License-Identifier: GPL-2.0-only
 * Facts: public Linux dpu_7_0_sm8350.h, dpu_hw_{ctl,sspp,intf,lm}.c,
 * dsi.xml, dsi_phy_7nm.xml, dispcc-sm8250.c's SM8350-specific branch;
 * public OnePlus SM8350 R11 sde_hw_{ctl,sspp,top}.c and actual HDK DT.
 * Registers are normalized to DPU=MDSS+0x1000. This file has no writes,
 * ownership transfer, resource enables, delay or IRQ acknowledgment. */
#include <Library/Hdk8350BootDisplayCrLib.h>
#define BIT(n) (1U << (n))
#define DPU 0x0ae01000ULL
STATIC CONST UINT32 Sources[]={0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000};
STATIC VOID Zero(VOID *Buffer, UINTN Bytes) {
  UINT8 *P=Buffer; for (UINTN I=0; I<Bytes; ++I) P[I]=0;
}
STATIC CR_STATUS Read(CONST CR_HDK8350_DISPLAY_IO *Io,
                      CR_HDK8350_DISPLAY_OBSERVATION *O, UINT64 Address, UINT32 *Value) {
  if (O->Reads>=CR_HDK8350_DISPLAY_MAX_READS) return CR_BAD_BUFFER_SIZE;
  ++O->Reads;
  CR_STATUS S=Io->Read32(Io->Context,Address,Value);
  return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;
}
#define GET(a,v) do {CR_STATUS S=Read(Io,O,(a),&(v));if(S!=CR_SUCCESS)return S;} while(0)
STATIC CR_STATUS Sweep(CONST CR_HDK8350_DISPLAY_IO *Io,
                      CR_HDK8350_DISPLAY_OBSERVATION *O, CR_HDK8350_DISPLAY_RAW *R) {
  Zero(R,sizeof(*R));
  GET(0x0ae00000ULL,R->DpuVersion); GET(DPU+4,R->InterfaceSelect);
  GET(DPU+0x2f4,R->SplitEnable); GET(DPU+0x2f8,R->SplitUpper);
  GET(DPU+0x3f0,R->SplitLower); GET(DPU+0x450,R->DceSelect);
  CONST UINT32 Ppb[]={0x330,0x338,0x370,0x374};
  for(UINT32 I=0;I<4;++I) GET(DPU+Ppb[I],R->PpbControl[I]);
  GET(DPU+0x334,R->PpbConfig[0]); GET(DPU+0x33c,R->PpbConfig[1]);
  for(UINT32 C=0;C<6;++C) {
    UINT64 B=DPU+0x15000+C*0x1000; CR_HDK8350_DISPLAY_CTL *T=&R->Ctl[C];
    GET(B+0x14,T->Top); GET(B+0xf4,T->Intf); GET(B+0xe8,T->Dsc);
    GET(B+0xec,T->Wb); GET(B+0xf0,T->Cwb); GET(B+0xf8,T->Cdm);
    GET(B+0xe4,T->Merge); GET(B+0x18,T->Flush); GET(B+0x30,T->Reset);
    GET(B+0x134,T->IntfMaster); GET(B+0xfc,T->Fetch);
    for(UINT32 L=0;L<6;++L) {
      GET(B+(L==5?0x24:L*4),T->Layer[L][0]);
      GET(B+0x40+L*4,T->Layer[L][1]); GET(B+0x70+L*4,T->Layer[L][2]);
      GET(B+0xa0+L*4,T->Layer[L][3]);
    }
    GET(DPU+0x44000+C*0x1000,R->LmMode[C]);
    GET(DPU+0x44004+C*0x1000,R->LmOutput[C]);
  }
  for(UINT32 I=0;I<8;++I) {
    UINT64 B=DPU+Sources[I]; CR_HDK8350_DISPLAY_SOURCE *F=&R->Source[I];
    GET(B,F->SourceSize); GET(B+8,F->SourceXy); GET(B+0xc,F->OutputSize);
    GET(B+0x10,F->OutputXy);
    for(UINT32 J=0;J<4;++J) GET(B+0x14+J*4,F->Address[J]);
    GET(B+0x24,F->Stride[0]); GET(B+0x28,F->Stride[1]);
    GET(B+0x30,F->Format); GET(B+0x34,F->Unpack); GET(B+0x38,F->OpMode);
    GET(B+0x70,F->Secure); GET(B+0x170,F->MultiRect);
    GET(B+0xb4,F->Decimation); GET(B+0x40,F->Exclusion);
    for(UINT32 J=0;J<3;++J) for(UINT32 K=0;K<3;++K)
      GET(B+0x100+J*0x10+K*4,F->PixelExtension[J*3+K]);
    /* QSEED3 and CSC10 sub-blocks exist on VIG only, not DMA pipes. */
    if(I<4) {GET(B+0xa04,F->Scaler); GET(B+0x1a00,F->Csc);}
  }
  for(UINT32 I=0;I<4;++I) {
    UINT64 B=DPU+0x34000+I*0x1000; CR_HDK8350_DISPLAY_INTF *T=&R->Intf[I];
    GET(B,T->Enable); GET(B+4,T->Config); GET(B+0x60,T->Config2);
    GET(B+0x26c,T->Status); GET(B+8,T->Hsync); GET(B+0xc,T->VPeriod);
    GET(B+0x14,T->VPulse); GET(B+0x3c,T->HDisplay);
    GET(B+0x1c,T->VStart); GET(B+0x24,T->VEnd); GET(B+0x40,T->HActive);
    GET(B+0x2c,T->VActiveStart); GET(B+0x34,T->VActiveEnd);
    GET(B+0x64,T->DataHDisplay); GET(B+0x68,T->DataHActive);
    GET(B+0x50,T->Polarity); GET(B+0x90,T->PanelFormat); GET(B+0xa8,T->CountEnable);
    GET(B+0xac,T->FrameCount); GET(B+0xb0,T->LineCount); GET(B+0x25c,T->Mux);
    GET(B+0x84,T->CommandTrigger); GET(B+0x100,T->TestEnable);
    if(I==1||I==2) {GET(B+0x284,T->TearEnable); GET(B+0x2b4,T->AutoRefresh);}
  }
  for(UINT32 I=0;I<2;++I) {
    UINT64 B=I?0x0ae96000ULL:0x0ae94000ULL;
    CR_HDK8350_DISPLAY_HOST *H=&R->Host[I]; CR_HDK8350_DISPLAY_PHY *P=&R->Phy[I];
    GET(B,H->Version); GET(B+4,H->Control); GET(B+8,H->Status);
    GET(B+0x10,H->VideoConfig); GET(B+0x24,H->ActiveH); GET(B+0x28,H->ActiveV);
    GET(B+0x2c,H->Total); GET(B+0x330,H->Split); GET(B+0xac,H->LaneControl);
    GET(B+0xb0,H->LaneSwap);
    GET(B+0x2d4,H->Cphy); GET(B+0x118,H->Reset);
    B+=0x400;
    GET(B+0x24,P->Control); GET(B+0x3c,P->Pll); GET(B+0x6b0,P->PllStatus);
    GET(B+0x14,P->Clock); GET(B+0x1c,P->Buffer); GET(B+0xa0,P->Lanes);
    GET(B+0xa4,P->ClockLane); GET(B+0x140,P->Status);
    GET(B+0x18,P->Global); GET(B+0x34,P->LaneMap[0]); GET(B+0x38,P->LaneMap[1]);
  }
  GET(0x0af03000ULL,R->DispGdsc);
  CONST UINT64 Gcc[]={0x127004,0x12701c,0x12700c,0x127014};
  CONST UINT32 Cc[]={0x207c,0x200c,0x201c,0x2024,0x2028,0x202c,0x2004};
  for(UINT32 I=0;I<4;++I) GET(Gcc[I],R->GccDisplay[I]);
  for(UINT32 I=0;I<7;++I) GET(0x0af00000ULL+Cc[I],R->DispClock[I]);
  /* SM8350 probe subtracts four bytes from these SM8250 RCG/divider bases.
   * CFG is CMD+4, hence BYTE0_CFG=2110 and PCLK0_CFG=2098. */
  GET(0x0af02110ULL,R->DsiClockConfig[0]); GET(0x0af02098ULL,R->DsiClockConfig[1]);
  GET(0x0af02124ULL,R->DsiClockConfig[2]);
  GET(0x0af0210cULL,R->DsiClockCommand[0]); GET(0x0af02094ULL,R->DsiClockCommand[1]);
  return CR_SUCCESS;
}
#undef GET

STATIC BOOLEAN Same(CONST CR_HDK8350_DISPLAY_RAW *A, CR_HDK8350_DISPLAY_RAW *B) {
  for(UINT32 I=0;I<4;++I) {
    B->Intf[I].FrameCount=A->Intf[I].FrameCount;
    B->Intf[I].LineCount=A->Intf[I].LineCount;
    /* STATUS bit0 is timing enable; other hardware status is dynamic. */
    B->Intf[I].Status=(B->Intf[I].Status&1)|(A->Intf[I].Status&~1U);
  }
  /* Command-engine/DMA/MDP busy bits must remain unchanged and clear.
   * The video engine's live status can legitimately change while sampled. */
  for(UINT32 I=0;I<2;++I) {
    UINT32 Mask=I?0x8000001fU:0x80000007U;
    B->Host[I].Status=(B->Host[I].Status&Mask)|(A->Host[I].Status&~Mask);
  }
  CONST UINT8 *X=(CONST UINT8 *)A,*Y=(CONST UINT8 *)B;
  for(UINTN I=0;I<sizeof(*A);++I) if(X[I]!=Y[I]) return FALSE;
  return TRUE;
}
CR_STATUS CrHdk8350DisplayReadInventory(CONST CR_HDK8350_DISPLAY_IO *Io,
                                     CR_HDK8350_DISPLAY_OBSERVATION *O)
{
  if(!O) return CR_INVALID_PARAMETER;
  Zero(O,sizeof(*O));
  if(!Io||!Io->Read32) return CR_INVALID_PARAMETER;
  CR_STATUS S=Sweep(Io,O,&O->Raw); if(S!=CR_SUCCESS) return S;
  CR_HDK8350_DISPLAY_RAW Other;
  S=Sweep(Io,O,&Other); if(S!=CR_SUCCESS) return S;
  /* Preserve latest dynamic counters, then compare only configuration. */
  for(UINT32 I=0;I<4;++I) {
    O->Raw.Intf[I].FrameCount=Other.Intf[I].FrameCount;
    O->Raw.Intf[I].LineCount=Other.Intf[I].LineCount;
  }
  O->Stable=Same(&O->Raw,&Other);
  O->Revision=CR_HDK8350_DISPLAY_REVISION;
  return CR_SUCCESS;
}

STATIC VOID ClearDerived(CR_HDK8350_DISPLAY_OBSERVATION *O) {
  O->Width=O->Height=O->Stride=O->SourceOffset=O->Format=O->Unpack=0;
  O->RedMask=O->GreenMask=O->BlueMask=O->ReservedMask=0;
  O->Device=O->Bytes=0;
}
CR_STATUS CrHdk8350DisplayAdmitPreserved(CR_HDK8350_DISPLAY_OBSERVATION *O)
{
  if(!O) return CR_INVALID_PARAMETER;
  ClearDerived(O);
  if(O->Revision!=CR_HDK8350_DISPLAY_REVISION||!O->Stable) return CR_NOT_READY;
  CONST CR_HDK8350_DISPLAY_RAW *R=&O->Raw;
  if((R->DpuVersion&0xffff0000U)!=0x70000000U) return CR_UNSUPPORTED;
  if(R->SplitEnable||R->SplitUpper||R->SplitLower||R->DceSelect) return CR_UNSUPPORTED;
  for(UINT32 I=0;I<4;++I) if(R->PpbControl[I]) return CR_UNSUPPORTED;
  for(UINT32 I=0;I<2;++I) if(R->PpbConfig[I]) return CR_UNSUPPORTED;
  for(UINT32 C=0;C<6;++C) {
    CONST CR_HDK8350_DISPLAY_CTL *T=&R->Ctl[C];
    if(T->Flush||T->Reset) return CR_NOT_READY;
    if(T->Dsc||T->Wb||T->Cwb||T->Cdm||T->Merge||T->IntfMaster) return CR_UNSUPPORTED;
    /* DPU7's public group_id F disables VM CTL collating; video has CMD bit17 clear. */
    if(C==0) {if(T->Top!=0xf0000000U||T->Intf!=2) return CR_UNSUPPORTED;}
    else if((T->Top&&T->Top!=0xf0000000U)||T->Intf||T->Fetch) return CR_UNSUPPORTED;
    for(UINT32 L=0;L<6;++L) for(UINT32 W=0;W<4;++W)
      if((C||L||W)&&T->Layer[L][W]) return CR_UNSUPPORTED;
  }
  /* Only one base-stage physical VIG0 or DMA0, with optional border-out bit.
   * Other stage/extension bits imply a different or blended route. */
  UINT32 Layer=R->Ctl[0].Layer[0][0]&~BIT(24), Index;
  if(Layer==1) Index=0;
  else if(Layer==BIT(18)) Index=4;
  else return CR_UNSUPPORTED;
  if(R->Ctl[0].Fetch!=(Index?1U:BIT(16))) return CR_UNSUPPORTED;
  CONST UINT32 Size=0x04380780U;
  CONST CR_HDK8350_DISPLAY_SOURCE *F=&R->Source[Index];
  if(F->SourceSize!=Size||F->OutputSize!=Size||F->SourceXy||F->OutputXy||
     R->LmOutput[0]!=Size||(R->LmMode[0]&BIT(31))||F->Decimation||F->MultiRect||
     F->Secure||F->Exclusion||F->Scaler||F->Csc||
     (F->OpMode&~BIT(31))||F->Format!=0x236ffU) return CR_UNSUPPORTED;
  if(F->OpMode&BIT(31)) for(UINT32 I=0;I<3;++I)
    if(F->PixelExtension[I*3]||F->PixelExtension[I*3+1]||
       F->PixelExtension[I*3+2]!=Size) return CR_UNSUPPORTED;
  UINT32 Masks[4]={0}, Seen=0;
  for(UINT32 I=0;I<4;++I) {
    UINT32 C=(F->Unpack>>(8*I))&255U;
    if(C>3||(Seen&BIT(C))) return CR_UNSUPPORTED;
    Seen|=BIT(C); Masks[C]=0xffU<<(8*I);
  }
  UINT64 Bytes=(UINT64)F->Stride[0]*1080U, Device=F->Address[0];
  if(F->Stride[0]<7680||F->Stride[0]>65535||(F->Stride[0]&3)||F->Stride[1]||
     !Device||(Device&3)||F->Address[1]||F->Address[2]||F->Address[3]||
     Bytes>0x100000000ULL-Device) return CR_UNSUPPORTED;
  for(UINT32 I=0;I<4;++I) {
    CONST CR_HDK8350_DISPLAY_INTF *T=&R->Intf[I];
    if(T->TearEnable||T->AutoRefresh||T->CommandTrigger||T->TestEnable) return CR_UNSUPPORTED;
    if(I!=1) {if(T->Enable||(T->Status&1)) return CR_UNSUPPORTED; continue;}
    if(T->Enable!=1||!(T->Status&1)||T->Mux||
       (T->Config&~BIT(31))||T->Config2!=BIT(4)||T->Hsync!=0x0898002cU||
       T->VPeriod!=2475000||T->VPulse!=11000||T->HDisplay!=0x083f00c0U||
       T->VStart!=90200||T->VEnd!=2466199||T->HActive||T->VActiveStart||
       T->VActiveEnd||T->DataHDisplay!=0x083f00c0U||T->DataHActive||T->Polarity||
       T->PanelFormat!=0x213f||T->CountEnable!=3) return CR_UNSUPPORTED;
  }
  CONST CR_HDK8350_DISPLAY_HOST *H=&R->Host[0];
  if((H->Version!=0x20050000U&&H->Version!=0x20050001U)||
     (H->Control&0x1f7)!=0x1f3||(H->Status&0x80000007U)||
     (H->VideoConfig&0x3ff)!=0x130||H->ActiveH!=0x084000c0U||
     H->ActiveV!=0x04610029U||H->Total!=0x04640897U||H->Split||H->Cphy||H->Reset||
     H->LaneSwap||(H->LaneControl&(BIT(28)|BIT(24)))!=BIT(28)||
     (R->Host[1].Control&7)||(R->Host[1].Status&0x8000001fU)) return CR_UNSUPPORTED;
  CONST CR_HDK8350_DISPLAY_PHY *P=&R->Phy[0];
  if((P->Control&0x7f)!=0x7f||!(P->Pll&1)||!(P->PllStatus&1)||
     !(P->Clock&BIT(5))||P->Buffer!=1||(P->Lanes&0x1f)!=0x1f||
     (P->ClockLane&0x60)!=0x60||!(P->Status&1)||
     (P->Global&BIT(6))||P->LaneMap[0]!=0x21||P->LaneMap[1]!=0x84||
     (R->Phy[1].Control&0x7f)||(R->Phy[1].Pll&1)||
     (R->Phy[1].Clock&BIT(5))||R->Phy[1].Buffer) return CR_UNSUPPORTED;
  if(!(R->DispGdsc&BIT(31))||(R->DispGdsc&1)) return CR_NOT_READY;
  for(UINT32 I=0;I<3;++I) if(!(R->GccDisplay[I]&1)) return CR_NOT_READY;
  for(UINT32 I=0;I<7;++I)
    if((R->DispClock[I]&(BIT(31)|1))!=1) return CR_NOT_READY;
  if((R->DsiClockConfig[0]&0x700)!=0x100||(R->DsiClockConfig[1]&0x700)!=0x100||
     (R->DsiClockCommand[0]&1)||(R->DsiClockCommand[1]&1)) return CR_NOT_READY;
  O->Width=1920; O->Height=1080; O->Stride=F->Stride[0]; O->SourceOffset=Sources[Index];
  O->Format=F->Format; O->Unpack=F->Unpack; O->Device=Device; O->Bytes=Bytes;
  /* Linux's component selectors: 0=G/Y, 1=B/Cb, 2=R/Cr, 3=A/X. */
  O->RedMask=Masks[2]; O->GreenMask=Masks[0]; O->BlueMask=Masks[1]; O->ReservedMask=Masks[3];
  return CR_SUCCESS;
}
