/* Read-only SM8250 inherited command-route inventory and admission.
 * Copyright (c) 2015-2020, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 * Facts/decoding: Android cad09e061ef6cf689a4a1e54d27562e0f042236a
 * sde_hw_{ctl,sspp,intf,pingpong,dsc,interrupts,reg_dma_v1}.c and HDK8245 FDT.
 * Offsets here are normalized to DPU=MDSS+0x1000; no write or IRQ ack occurs.
 */
#include "Internal.h"
#define DPU CrMdssBootDpu
#define TRY(E) do { CR_STATUS S=(E);if(S!=CR_SUCCESS)return S; } while(0)
#define READ(W,O,V) TRY(Read(Context,(W),(O),&(V)))

CONST UINT32 BootIrqOffsets[CR_MDSS_BOOT8250_IRQ_BANKS][3]={
  {0x10,0x14,0x18},{8,0xc,0x2c},
  {0x6d800,0x6d804,0x6d808},{0x6d900,0x6d904,0x6d908},
  {0x1c,0x20,0x24},
  {0x6a1c0,0x6a1c4,0x6a1c8},{0x6a9c0,0x6a9c4,0x6a9c8},
  {0x6b1c0,0x6b1c4,0x6b1c8},{0x6b9c0,0x6b9c4,0x6b9c8},
  {0x6c1c0,0x6c1c4,0x6c1c8},
  {0x7e050,0x7e054,0x7e058},{0x7e150,0x7e154,0x7e158}
};
/* Kona disables AD4 only; INTF4 remains source-inventoried but has no FDT
 * route. LTM0/1 are real FDT blocks. All three are zero-only diagnostics. */
CONST UINT32 BootIrqAllowed[CR_MDSS_BOOT8250_IRQ_BANKS]={
  0x04000100,0x200,7,0,0,0,0,0,0,0,0,0
};
STATIC CONST UINT32 SourceOffsets[8]={
  0x4000,0x6000,0x8000,0xa000,0x24000,0x26000,0x28000,0x2a000
};

typedef struct {UINT8 Word,Shift,Bits,ExtensionBit;} STAGE;
/* Exact Android ctl_sspp_stage_reg_map, physical then virtual rectangle.
 * Enumeration order includes unsupported RGB/cursor entries for exclusion.
 * 0xff means there is no secondary bit in CTL_LAYER_EXT. */
STATIC CONST STAGE StageMap[14][2]={
  {{0,0,3,0},{3,0,4,0xff}},{{0,3,3,2},{3,4,4,0xff}},
  {{0,6,3,4},{3,8,4,0xff}},{{0,26,3,6},{3,12,4,0xff}},
  {{0,9,3,8},{0,0,0,0xff}},{{0,12,3,10},{0,0,0,0xff}},
  {{0,15,3,12},{0,0,0,0xff}},{{0,29,3,14},{0,0,0,0xff}},
  {{0,18,3,16},{2,8,4,0xff}},{{0,21,3,18},{2,12,4,0xff}},
  {{2,0,4,0xff},{2,16,4,0xff}},{{2,4,4,0xff},{2,20,4,0xff}},
  {{1,20,4,0xff},{0,0,0,0xff}},{{0,26,4,0xff},{0,0,0,0xff}}
};
typedef struct {UINT32 Index,Offset,Xin,Force,Flush,Stride;UINT64 Device,Bytes;} ROUTE;

STATIC CR_STATUS Staged(CONST UINT32 Words[4],UINT32 *Index)
{
  UINT32 Count=0,Found=0,Known[4]={BOOTBIT(24),0,0,0};
  for(UINT32 I=0;I<14;++I)for(UINT32 R=0;R<2;++R){
    CONST STAGE *M=&StageMap[I][R];if(!M->Bits)continue;
    UINT32 Mask=((1U<<M->Bits)-1U)<<M->Shift;
    UINT32 Stage=(Words[M->Word]&Mask)>>M->Shift;
    Known[M->Word]|=Mask;
    if(M->ExtensionBit!=0xff){
      Known[1]|=BOOTBIT(M->ExtensionBit);
      if(Words[1]&BOOTBIT(M->ExtensionBit))Stage|=8;
    }
    if(!Stage)continue;
    /* Eleven hardware blend stages. Only one physical VIG0 or DMA0 may
     * participate; unknown enum/virtual encodings are not ignored. */
    if(Stage>11 || R || (I!=0 && I!=8))return CR_UNSUPPORTED;
    Found=I==0?0:4;++Count;
  }
  for(UINT32 I=0;I<4;++I)if(Words[I]&~Known[I])return CR_UNSUPPORTED;
  if(Count!=1 || (Words[0]&BOOTBIT(24)))return CR_UNSUPPORTED;
  *Index=Found;return CR_SUCCESS;
}

STATIC CR_STATUS Route(CONST CR_MDSS_BOOT_OBSERVATION *O,ROUTE *R)
{
  if((O->DpuVersion&0xffff0000U)!=0x60000000U || (O->MdssPending&~0x11U))
    return CR_UNSUPPORTED;
  for(UINT32 I=0;I<CR_MDSS_BOOT8250_IRQ_BANKS;++I)
    if((O->IrqEnable[I]|O->IrqStatus[I])&~BootIrqAllowed[I])return CR_UNSUPPORTED;
  if(O->RegDmaMode)return CR_UNSUPPORTED;
  for(UINT32 C=0;C<6;++C)for(UINT32 W=0;W<4;++W)
    if(O->RegDmaQueues[C][W])return CR_UNSUPPORTED;
  for(UINT32 I=0;I<5;++I)if(O->RegDmaStatus[I])return CR_UNSUPPORTED;
  for(UINT32 C=0;C<6;++C){
    CONST CR_MDSS_BOOT_CTL *T=&O->Ctl[C];
    if(T->Wb||T->Cwb||T->Cdm||T->Merge||T->Reset||T->RegDmaTrigger||
       T->IntfMaster||T->RotTop||T->RotStart)return CR_UNSUPPORTED;
    if(C==0){if(T->Top!=BOOTBIT(17)||T->Intf!=2||T->Dsc!=1)return CR_UNSUPPORTED;}
    else if(T->Top||T->Intf||T->Dsc||T->Flush)return CR_UNSUPPORTED;
    for(UINT32 L=0;L<6;++L)if(C||L)
      for(UINT32 W=0;W<4;++W)if(T->Layer[L][W])return CR_UNSUPPORTED;
  }
  TRY(Staged(O->Ctl[0].Layer[0],&R->Index));
  R->Offset=SourceOffsets[R->Index];R->Xin=R->Index==0?0:1;
  R->Force=BOOTBIT(R->Index==0?0:8);
  R->Flush=BOOTBIT(R->Index==0?0:11)|BOOTBIT(6)|BOOTBIT(17)|BOOTBIT(22)|BOOTBIT(31);
  /* A pending flush can separate shadow geometry from the live fetch. */
  if(O->Ctl[0].Flush)return CR_NOT_READY;
  /* No split-display/PP bridge or alternate DCE route is qualified here. */
  if(O->SplitEnable||O->SplitUpper||O->SplitLower||O->DceSelect)return CR_UNSUPPORTED;
  for(UINT32 I=0;I<4;++I)if(O->PpbControl[I])return CR_UNSUPPORTED;
  for(UINT32 I=0;I<2;++I)if(O->PpbConfig[I])return CR_UNSUPPORTED;
  for(UINT32 I=0;I<6;++I)
    if(O->PpDsc[I]!=(I==0?1U:0U)||O->PpConfig[I]||O->PpAutoRefresh[I])
      return CR_UNSUPPORTED;
  for(UINT32 I=0;I<4;++I){
    CONST CR_MDSS_BOOT_INTF *T=&O->Intf[I];
    if(T->Enable||(O->IntfStatus[I]&1)||T->SplitLink)return CR_UNSUPPORTED; /* no video timing engine */
    if(I==1){if(T->TeEnable!=1||T->Mux||T->CommandTrigger>1)return CR_UNSUPPORTED;}
    else if(T->TeEnable||T->AutoRefresh||T->CommandTrigger)return CR_UNSUPPORTED;
  }
  if((O->DscMux[0]&15)||O->DscWords[0][0]||
     O->DscWords[0][2]!=((1080U<<16)|2340U)||
     O->DscWords[0][3]!=((540U<<16)|20U))return CR_UNSUPPORTED;
  if((O->HostControl[0]&0x1f7)!=0x1f5 || (O->HostControl[1]&7) ||
     (O->HostSplitLink[0]&1)||(O->HostSplitLink[1]&1)||
     (O->HostStatus[0]&2)||(O->HostStatus[1]&6)||
     (O->HostInterrupt[0]&~0x03300f03U)||(O->HostInterrupt[1]&0xaaaaaaaaU))
    return CR_UNSUPPORTED;
  if((O->PhyPll[0]&1)!=1 || (O->PhyControl[0]&0x7f)!=0x7f ||
     !(O->PhyClock[0]&0x20)||(O->PhyPll[1]&1)||(O->PhyControl[1]&0x7f)||
     (O->PhyClock[1]&0x20)||O->PhyBuffer[1]||O->PhyRbuf[1])return CR_UNSUPPORTED;

  CONST CR_MDSS_BOOT_SSPP *F=&O->Sspp[R->Index];
  UINT32 Size=(2340U<<16)|1080U;
  if(F->SourceSize!=Size||F->OutputSize!=Size||F->SourceXy||F->OutputXy||
     O->LmOutput[0]!=Size||F->Decimation||F->MultiRect||(F->Secure&15)||
     F->OpMode!=BOOTBIT(31)||(F->Format&~BOOTBIT(8))!=0x236ffU)
    return CR_UNSUPPORTED;
  /* Packed 32-bit linear RGB(A/X), tight unpack; any channel permutation
   * is safe for byte-range proof, but duplicate/unknown selectors are not. */
  UINT32 Seen=0;
  for(UINT32 I=0;I<4;++I){UINT32 C=(F->Unpack>>(I*8))&0xff;
    if(C>3||(Seen&BOOTBIT(C)))return CR_UNSUPPORTED;Seen|=BOOTBIT(C);}
  for(UINT32 C=0;C<3;++C)
    if(F->PixelExtension[C*3]||F->PixelExtension[C*3+1]||
       F->PixelExtension[C*3+2]!=Size)return CR_UNSUPPORTED;
  R->Stride=F->Stride[0];R->Device=F->Address[0];R->Bytes=(UINT64)R->Stride*2340U;
  if(R->Stride<1080U*4U||R->Stride>0xffffU||(R->Stride&3)||F->Stride[1]||
     F->Address[1]||F->Address[2]||F->Address[3]||!R->Device||(R->Device&3)||
     R->Device+R->Bytes>0x100000000ULL)return CR_UNSUPPORTED;
  return CR_SUCCESS;
}

STATIC CR_STATUS Dma(CONST CR_MDSS_BOOT_DMA_OBSERVATION *D,CONST ROUTE *R)
{
  if(!D->ProducerGeneration||D->Reserved||!D->Count||D->Count>4||
     D->DeviceAddress!=R->Device||D->Bytes!=R->Bytes||
     D->PhysicalAddress<0x9c000000ULL||D->PhysicalAddress>=0x9e300000ULL||
     D->Bytes>0x9e300000ULL-D->PhysicalAddress)return CR_NOT_READY;
  CONST UINT32 Sid[4]={0x820,0x822,0xc20,0xc22};UINT32 Covered=0;
  for(UINT32 I=0;I<D->Count;++I){
    CONST CR_MDSS_BOOT_STREAM *S=&D->Streams[I];
    UINT32 Mask=(S->Smr>>16)&0x7fff,Id=S->Smr&0x7fff;
    if(!(S->Smr&BOOTBIT(31))||(S->Smr&BOOTBIT(15))||(Mask&~0x402U)||
       (Id&~0x402U)!=0x820||S->Reserved||S->Index>=128)return CR_UNSUPPORTED;
    for(UINT32 J=0;J<I;++J)if(S->Index==D->Streams[J].Index)return CR_UNSUPPORTED;
    UINT32 Type=(S->S2cr>>16)&3;
    if(Type>1 || (Type==0 && (S->Bank>=128||S->Bank!=(S->S2cr&0xff)||!(S->Sctlr&1))))
      return CR_UNSUPPORTED;
    for(UINT32 J=0;J<4;++J)if(!((Sid[J]^Id)&~Mask)){
      if(Covered&BOOTBIT(J))return CR_UNSUPPORTED;Covered|=BOOTBIT(J);
    }
  }
  return Covered==15?CR_SUCCESS:CR_UNSUPPORTED;
}

STATIC VOID ClearDerived(CR_MDSS_BOOT_OBSERVATION *O)
{
  O->SourceIndex=O->SourceOffset=O->Xin=O->ForceOffset=O->ForceMask=0;
  O->FlushMask=O->Width=O->Height=O->Stride=0;
  O->FrameUnpack=O->PixelRedMask=O->PixelGreenMask=0;
  O->PixelBlueMask=O->PixelReservedMask=0;
  O->FrameDevice=O->FrameBytes=0;
}

STATIC VOID Derive(CR_MDSS_BOOT_OBSERVATION *O,CONST ROUTE *R)
{
  O->SourceIndex=R->Index;O->SourceOffset=R->Offset;O->Xin=R->Xin;
  O->ForceOffset=0x2ac;O->ForceMask=R->Force;O->FlushMask=R->Flush;
  O->Width=1080;O->Height=2340;O->Stride=R->Stride;
  O->FrameDevice=R->Device;O->FrameBytes=R->Bytes;
  O->FrameUnpack=O->Sspp[R->Index].Unpack;
  /* Public Android sde_hw_mdss.h C0_G_Y/C1_B_Cb/C2_R_Cr/C3_ALPHA.
   * Byte I in source memory selects one component; Route already excludes
   * duplicate/unknown selectors. This is native GOP mask data, not a format
   * borrowed from a different panel or an assumed physical identity map. */
  for(UINT32 I=0;I<4;++I){
    UINT32 Mask=0xffU<<(I*8),Component=(O->FrameUnpack>>(I*8))&0xff;
    if(Component==0)O->PixelGreenMask=Mask;
    else if(Component==1)O->PixelBlueMask=Mask;
    else if(Component==2)O->PixelRedMask=Mask;
    else O->PixelReservedMask=Mask;
  }
}

CR_STATUS BootAdmit(CR_MDSS_BOOT_OBSERVATION *O)
{
  if(!O)return CR_INVALID_PARAMETER;
  /* A failed repeat admission cannot retain a formerly derived resource. */
  ClearDerived(O);
  if(O->Revision!=CR_MDSS_BOOT8250_REVISION)return CR_NOT_READY;
  ROUTE R={0};TRY(Route(O,&R));TRY(Dma(&O->Dma,&R));
  Derive(O,&R);return CR_SUCCESS;
}

CR_STATUS CrMdssBoot8250ProbePreservedRoute(CR_MDSS_BOOT_OBSERVATION *O)
{
  if(!O)return CR_INVALID_PARAMETER;
  ClearDerived(O);
  if(O->Revision!=CR_MDSS_BOOT8250_REVISION)return CR_NOT_READY;
  ROUTE R={0};TRY(Route(O,&R));
  /* A preserving GOP overwrites the sole base plane. A higher blend stage
   * could consume inherited LM blend/alpha state not qualified by this API. */
  UINT32 Shift=R.Index==0?0:18,Extension=R.Index==0?0:16;
  if(((O->Ctl[0].Layer[0][0]>>Shift)&7U)!=1U||
     (O->Ctl[0].Layer[0][1]&BOOTBIT(Extension)))return CR_UNSUPPORTED;
  /* Android sde_hw_intf_{setup,get}_autorefresh_config: bit31 enable and
   * low16 frame count. A command pipeline without automatic transfers needs
   * a real CTL_START owner before framebuffer writes can form a GOP. */
  UINT32 Refresh=O->Intf[1].AutoRefresh;
  if(!(Refresh&BOOTBIT(31))||!(Refresh&0xffffU)||(Refresh&0x7fff0000U)||
     (O->Sspp[R.Index].Format&BOOTBIT(8)))return CR_UNSUPPORTED;
  /* The preserving route cannot consume caller-supplied DMA observations.
   * Its SMMU mapping and reserved backing must be qualified independently. */
  Derive(O,&R);return CR_SUCCESS;
}

STATIC CR_STATUS ReadInventoryOnce(VOID *Context,CR_MDSS_BOOT_READ32 Read,
                                  CR_MDSS_BOOT_OBSERVATION *O)
{
  cr_memset(O,0,sizeof(*O));
  READ(CrMdssBootTop,0,O->DpuVersion);READ(CrMdssBootTop,0x10,O->MdssPending);
  READ(DPU,4,O->InterfaceSelect);READ(DPU,0x2f4,O->SplitEnable);
  READ(DPU,0x2f8,O->SplitUpper);READ(DPU,0x3f0,O->SplitLower);
  READ(DPU,0x450,O->DceSelect);
  CONST UINT32 Ppb[]={0x330,0x338,0x370,0x374};
  for(UINT32 I=0;I<4;++I)READ(DPU,Ppb[I],O->PpbControl[I]);
  READ(DPU,0x334,O->PpbConfig[0]);READ(DPU,0x33c,O->PpbConfig[1]);
  for(UINT32 C=0;C<6;++C){
    UINT32 B=0x1000+C*0x200;CR_MDSS_BOOT_CTL *T=&O->Ctl[C];
    READ(DPU,B+0x14,T->Top);READ(DPU,B+0xf4,T->Intf);READ(DPU,B+0xe8,T->Dsc);
    READ(DPU,B+0xec,T->Wb);READ(DPU,B+0xf0,T->Cwb);READ(DPU,B+0xf8,T->Cdm);
    READ(DPU,B+0xe4,T->Merge);READ(DPU,B+0x18,T->Flush);READ(DPU,B+0x30,T->Reset);
    READ(DPU,B+0xd4,T->RegDmaTrigger);READ(DPU,B+0x134,T->IntfMaster);
    READ(DPU,B+0xc0,T->RotTop);READ(DPU,B+0xcc,T->RotStart);
    for(UINT32 L=0;L<6;++L){
      READ(DPU,B+(L==5?0x24:L*4),T->Layer[L][0]);
      READ(DPU,B+0x40+L*4,T->Layer[L][1]);READ(DPU,B+0x70+L*4,T->Layer[L][2]);
      READ(DPU,B+0xa0+L*4,T->Layer[L][3]);
    }
    UINT32 Q=0x70000+C*0x800;
    READ(DPU,Q+0xa0,O->PpDsc[C]);READ(DPU,Q,O->PpConfig[C]);
    READ(DPU,Q+0x30,O->PpAutoRefresh[C]);READ(DPU,0x44004+C*0x1000,O->LmOutput[C]);
  }
  for(UINT32 I=0;I<8;++I){
    UINT32 B=SourceOffsets[I];CR_MDSS_BOOT_SSPP *F=&O->Sspp[I];
    READ(DPU,B,F->SourceSize);READ(DPU,B+8,F->SourceXy);
    READ(DPU,B+0xc,F->OutputSize);READ(DPU,B+0x10,F->OutputXy);
    for(UINT32 J=0;J<4;++J)READ(DPU,B+0x14+J*4,F->Address[J]);
    READ(DPU,B+0x24,F->Stride[0]);READ(DPU,B+0x28,F->Stride[1]);
    READ(DPU,B+0x30,F->Format);READ(DPU,B+0x34,F->Unpack);READ(DPU,B+0x38,F->OpMode);
    READ(DPU,B+0x70,F->Secure);READ(DPU,B+0x170,F->MultiRect);
    READ(DPU,B+0xb4,F->Decimation);
    for(UINT32 C=0;C<3;++C)for(UINT32 J=0;J<3;++J)
      READ(DPU,B+0x100+C*0x10+J*4,F->PixelExtension[C*3+J]);
  }
  for(UINT32 I=0;I<4;++I){
    UINT32 B=0x6a000+I*0x800;CR_MDSS_BOOT_INTF *T=&O->Intf[I];
    READ(DPU,B,T->Enable);READ(DPU,B+4,T->Config);READ(DPU,B+0x284,T->TeEnable);
    READ(DPU,B+0x288,T->TeConfig);READ(DPU,B+0x2b4,T->AutoRefresh);
    READ(DPU,B+0x2b0,T->LineCount);READ(DPU,B+0x25c,T->Mux);
    READ(DPU,B+0x60,T->Config2);READ(DPU,B+0x80,T->SplitLink);
    READ(DPU,B+0x84,T->CommandTrigger);READ(DPU,B+0x26c,O->IntfStatus[I]);
    for(UINT32 J=0;J<4;++J)READ(DPU,0x80000+I*0x400+J*4,O->DscWords[I][J]);
    READ(DPU,0x81800+I*4,O->DscMux[I]);
    READ(DPU,0x2ac+I*8,O->Force[I]);
  }
  for(UINT32 I=0;I<2;++I){
    CR_MDSS_BOOT_WINDOW H=I?CrMdssBootHost1:CrMdssBootHost0;
    CR_MDSS_BOOT_WINDOW Y=I?CrMdssBootPhy1:CrMdssBootPhy0;
    READ(H,4,O->HostControl[I]);READ(H,8,O->HostStatus[I]);
    READ(H,0x110,O->HostInterrupt[I]);READ(H,0x330,O->HostSplitLink[I]);
    READ(Y,0x3c,O->PhyPll[I]);READ(Y,0x24,O->PhyControl[I]);
    READ(Y,0x14,O->PhyClock[I]);READ(Y,0x528,O->PhyBuffer[I]);
    READ(Y,0x1c,O->PhyRbuf[I]);
  }
  for(UINT32 I=0;I<CR_MDSS_BOOT8250_IRQ_BANKS;++I){
    READ(DPU,BootIrqOffsets[I][0],O->IrqEnable[I]);
    READ(DPU,BootIrqOffsets[I][1],O->IrqStatus[I]);
  }
  READ(CrMdssBootRegDma,4,O->RegDmaMode);
  for(UINT32 C=0;C<6;++C)for(UINT32 W=0;W<4;++W)
    READ(CrMdssBootRegDma,0x14+C*0x10+W*4,O->RegDmaQueues[C][W]);
  for(UINT32 I=0;I<5;++I)READ(CrMdssBootRegDma,0x160+I*4,O->RegDmaStatus[I]);
  READ(CrMdssBootVbif,0x200,O->VbifRequest);READ(CrMdssBootVbif,0x204,O->VbifAck);
  O->Revision=CR_MDSS_BOOT8250_REVISION;return CR_SUCCESS;
}

STATIC CR_STATUS ReadOwned(VOID *Context,CR_MDSS_BOOT_WINDOW W,UINT32 Offset,UINT32 *Value)
{
  return BootRead(Context,W,Offset,Value);
}

CR_STATUS BootObserve(CR_MDSS_BOOT8250 *P,CR_MDSS_BOOT_OBSERVATION *O)
{
  if(!P||!O)return CR_INVALID_PARAMETER;
  /* BootRead retains the previous production Ready validation before each
   * MMIO read. The preserving reader never supplies stand-in lease callbacks. */
  CR_STATUS S=ReadInventoryOnce(P,ReadOwned,O);
  if(S!=CR_SUCCESS)return S;
  O->Revision=0;
  ROUTE R={0};
  if(Route(O,&R)==CR_SUCCESS){
    TRY(BootReady(P,DPU));
    CR_STATUS S=P->Io.ObserveDma(P->Io.Context,R.Device,R.Bytes,&O->Dma);
    if(S!=CR_SUCCESS)return CR_ERROR(S)?S:CR_NOT_READY;
  }
  /* Complete unsupported snapshots remain diagnostic. No partial read can
   * set this marker, and only BootAdmit publishes derived resource identity. */
  O->Revision=CR_MDSS_BOOT8250_REVISION;return CR_SUCCESS;
}

STATIC CR_STATUS SameInventory(CONST CR_MDSS_BOOT_OBSERVATION *Old,
                               CONST CR_MDSS_BOOT_OBSERVATION *New)
{
  if(!Old||!New)return CR_INVALID_PARAMETER;
  if(Old->Revision!=CR_MDSS_BOOT8250_REVISION||New->Revision!=CR_MDSS_BOOT8250_REVISION)
    return CR_NOT_READY;
  for(UINT32 I=0;I<2;++I)
    if((Old->HostInterrupt[I]^New->HostInterrupt[I])&0xaaaaaaaaU)return CR_NOT_READY;
  CONST UINT8 *A=(CONST UINT8 *)Old,*B=(CONST UINT8 *)New;
  /* Compare future fields and padding by default. Only these explicitly
   * named dynamic/derived fields are exempt. Route/Dma rederive and validate
   * their source facts above; caller-supplied derived identities are ignored.
   * Real member addresses avoid null-pointer offset tricks on either OS. */
#define IN_MEMBER(M) (I>=(UINTN)((CONST UINT8 *)&Old->M-A) && \
                      I<(UINTN)((CONST UINT8 *)&Old->M-A)+sizeof(Old->M))
  for(UINTN I=0;I<sizeof(*Old);++I){
    if(IN_MEMBER(MdssPending)||IN_MEMBER(IrqStatus)||IN_MEMBER(HostStatus)||
       IN_MEMBER(HostInterrupt)||IN_MEMBER(SourceIndex)||IN_MEMBER(SourceOffset)||
       IN_MEMBER(Xin)||IN_MEMBER(ForceOffset)||IN_MEMBER(ForceMask)||
       IN_MEMBER(FlushMask)||IN_MEMBER(Width)||IN_MEMBER(Height)||IN_MEMBER(Stride)||
       IN_MEMBER(FrameUnpack)||IN_MEMBER(PixelRedMask)||IN_MEMBER(PixelGreenMask)||
       IN_MEMBER(PixelBlueMask)||IN_MEMBER(PixelReservedMask)||
       IN_MEMBER(FrameDevice)||IN_MEMBER(FrameBytes))continue;
    BOOLEAN Dynamic=FALSE;
    for(UINT32 N=0;N<4;++N)if(IN_MEMBER(Intf[N].LineCount)){Dynamic=TRUE;break;}
    if(!Dynamic&&A[I]!=B[I])return CR_NOT_READY;
  }
#undef IN_MEMBER
  return CR_SUCCESS;
}

CR_STATUS CrMdssBoot8250ReadInventory(VOID *Context,CR_MDSS_BOOT_READ32 Read,
  CR_MDSS_BOOT_OBSERVATION *Out)
{
  if(!Out)return CR_INVALID_PARAMETER;
  cr_memset(Out,0,sizeof(*Out));
  if(!Read)return CR_INVALID_PARAMETER;
  CR_MDSS_BOOT_OBSERVATION Repeat;
  CR_STATUS S=ReadInventoryOnce(Context,Read,Out);
  if(S==CR_SUCCESS)S=ReadInventoryOnce(Context,Read,&Repeat);
  if(S==CR_SUCCESS)S=SameInventory(Out,&Repeat);
  if(S!=CR_SUCCESS){cr_memset(Out,0,sizeof(*Out));return CR_ERROR(S)?S:CR_NOT_READY;}
  /* Publish the latest dynamic status after two unchanged static inventories. */
  *Out=Repeat;return CR_SUCCESS;
}

CR_STATUS BootSameRoute(CONST CR_MDSS_BOOT_OBSERVATION *Old,
                         CONST CR_MDSS_BOOT_OBSERVATION *New)
{
  if(!Old||!New)return CR_INVALID_PARAMETER;
  if(Old->Revision!=CR_MDSS_BOOT8250_REVISION||New->Revision!=CR_MDSS_BOOT8250_REVISION)
    return CR_NOT_READY;
  ROUTE R={0};TRY(Route(Old,&R));TRY(Dma(&Old->Dma,&R));
  TRY(Route(New,&R));TRY(Dma(&New->Dma,&R));
  return SameInventory(Old,New);
}
