/* Qualified HDK display resource ownership. SPDX-License-Identifier: GPL-2.0-only */
#include "MdssPlatformInternal.h"

CONST CR_MDSS_PLATFORM_CLOCK CrMdssPlatformClocks[CR_MDSS_PLATFORM_CLOCKS] = {
  {0,"gcc_disp_ahb_clk",0,FALSE}, {0,"gcc_disp_xo_clk",0,FALSE},
  {4,"disp_cc_mdss_ahb_clk",19200000,FALSE},
  {4,"disp_cc_mdss_mdp_clk",200000000,FALSE},
  {0,"gcc_disp_hf_axi_clk",0,FALSE}, {0,"gcc_disp_sf_axi_clk",0,FALSE},
  {4,"disp_cc_mdss_mdp_lut_clk",200000000,FALSE},
  {4,"disp_cc_mdss_vsync_clk",19200000,FALSE},
  {4,"disp_cc_mdss_esc0_clk",19200000,FALSE},
  {4,"disp_cc_mdss_core_gdsc",0,TRUE}
};
CONST CR_MDSS_PLATFORM_RAIL CrMdssPlatformRails[CR_MDSS_PLATFORM_RAILS] = {
  {"ldob6",1200,7}, {"ldob5",880,7},
  /* Schematic HDMI level shifters use VREG_S10B_1P8. L5B's qualified
     Sm8450Rail graph separately retains BOB3296/AUTO6 for bridge 3.3V. */
  {"smpb10",1800,6}
};
CONST CR_MDSS_PLATFORM_PATH CrMdssPlatformPaths[CR_MDSS_PLATFORM_PATHS] = {
  {"qcom,sm8450-mdp",0x4002,0x4003}, {"qcom,sm8450-mdp",0x4000,0x4001}
};
CONST UINT16 CrMdssPlatformSids[4] = {0x2800,0x2802,0x2c00,0x2c02};

STATIC VOID Zero(VOID *Buffer, UINTN Bytes) {
  UINT8 *P = Buffer; for (UINTN I=0; I<Bytes; ++I) P[I]=0;
}
STATIC CR_STATUS Exact(CR_STATUS S) {
  return S==CR_SUCCESS||CR_ERROR(S)?S:CR_NOT_READY;
}
STATIC CR_STATUS Check(CR_MDSS_PLATFORM *P) {
  return P != NULL && P->Initialized ? CR_SUCCESS : CR_INVALID_PARAMETER;
}
STATIC CR_STATUS CheckOwner(CR_MDSS_PLATFORM *P, VOID *Owner) {
  if (CR_ERROR(Check(P)) || Owner == NULL) return CR_INVALID_PARAMETER;
  return !P->PostOwned && P->Owner == Owner ? CR_SUCCESS : CR_BUSY;
}
STATIC BOOLEAN SameWitness(CONST CR_MDSS_PLATFORM_WITNESS *A, CONST CR_MDSS_PLATFORM_WITNESS *B) {
  return A->BootGeneration==B->BootGeneration && A->NativeTransferId==B->NativeTransferId &&
    A->DmaIncarnation==B->DmaIncarnation && A->SerialIncarnation==B->SerialIncarnation &&
    A->ExternalClockIncarnation==B->ExternalClockIncarnation && A->PermanentStoreId==B->PermanentStoreId;
}
STATIC CR_STATUS Qualify(CR_MDSS_PLATFORM *P, CR_MDSS_PLATFORM_WITNESS *Witness) {
  Zero(Witness,sizeof(*Witness));
  CR_STATUS S=Exact(P->Io.Qualify(P->Io.Context,&P->Config,Witness));
  if (CR_ERROR(S)) return S;
  if (Witness->Revision != CR_MDSS_PLATFORM_REVISION || Witness->Facts != CR_MDSS_QUAL_ALL ||
      Witness->BootGeneration != P->Config.BootGeneration || Witness->NativeTransferId == 0 ||
      Witness->DmaIncarnation == 0 || Witness->SerialIncarnation == 0 ||
      Witness->ExternalClockIncarnation == 0 || Witness->PermanentStoreId == 0)
    return CR_NOT_READY;
  return Exact(P->Io.ResourceReady(P->Io.Context,P));
}
CR_STATUS CrMdssPlatformInit(CR_MDSS_PLATFORM *P, CONST CR_MDSS_PLATFORM_IO *Io,
                             CONST CR_MDSS_PLATFORM_CONTRACT *C) {
  if (P == NULL || Io == NULL || C == NULL) return CR_INVALID_PARAMETER;
  if (P->Initialized) return CR_ALREADY_STARTED;
  if (!Io->Qualify || !Io->NativeClaim || !Io->NativeFence || !Io->NativeRelease || !Io->ResourceReady ||
      !Io->Clock || !Io->Rail || !Io->Bandwidth || !Io->ExternalClocks || !Io->Read32 ||
      !Io->Write32 || !Io->Fence || !Io->DelayUs || !Io->MapFrameBuffer ||
      !Io->UnmapFrameBuffer || !Io->Gpio || !Io->I2cWrite || !Io->I2cRead ||
      !Io->CommitPost || !Io->ReclaimPost) return CR_INVALID_PARAMETER;
  if (Io->PhyBytes<CR_MDSS_PHY_BYTES || Io->DispCcBytes<CR_MDSS_DISPCC_BYTES)
    return CR_BAD_BUFFER_SIZE;
  if (C->Revision != CR_MDSS_PLATFORM_REVISION || C->Board != CR_MDSS_PLATFORM_HDK8450 ||
      C->BootGeneration == 0 || C->DpuPhysical != 0xae01000 || C->DsiPhysical != 0xae94000 ||
      (C->FrameBufferPhysical & 0xfff) != 0 || C->FrameBufferPhysical == 0 ||
      C->FrameBufferBytes == 0 || C->FrameBufferBytes > 0x2b00000 ||
      C->FrameBufferBytes > (UINTN)-1 - 0xfff ||
      C->FrameBufferPhysical > (UINT64)-1 - ((C->FrameBufferBytes + 0xfff) & ~(UINTN)0xfff))
    return CR_INVALID_PARAMETER;
  CONST CR_MDSS_MODE *M=&C->Mode;
  if (M->Soc != CR_MDSS_SOC_SM8450 || M->Controller != 0 || M->Width != 1920 ||
      M->Height != 1080 || M->Stride != 1920*4 || M->HSync != 44 || M->HBackPorch != 148 ||
      M->HFrontPorch != 88 || M->VSync != 5 || M->VBackPorch != 36 || M->VFrontPorch != 4 ||
      M->RefreshQ16 != (60U<<16) || M->Lanes != 4 || M->VirtualChannel != 0 ||
      M->TrafficMode != 1 || M->VideoFlags != CR_MDSS_VIDEO_LP_MASK ||
      M->InvertHsync || M->InvertVsync || M->HsaHseAfterVsVe || M->DisableEot || M->BorderColor != 0)
    return CR_UNSUPPORTED;
  CR_STATUS S=CrMdssValidateMode(M,C->FrameBufferBytes); if (CR_ERROR(S)) return S;
  Zero(P,sizeof(*P)); P->Io=*Io;
  /* Build a zero-padded immutable config for bytewise ABI comparisons. */
  P->Config.Revision=C->Revision; P->Config.Board=C->Board; P->Config.Mode=*M;
  P->Config.DpuPhysical=C->DpuPhysical; P->Config.DsiPhysical=C->DsiPhysical;
  P->Config.FrameBufferPhysical=C->FrameBufferPhysical; P->Config.FrameBufferBytes=C->FrameBufferBytes;
  P->Config.BootGeneration=C->BootGeneration;
  P->PixelRate=148500000; P->Initialized=TRUE; return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformQuery(CR_MDSS_PLATFORM *P, CR_MDSS_PLATFORM_CONTRACT *Config) {
  if (Config == NULL || CR_ERROR(Check(P))) return CR_INVALID_PARAMETER;
  Zero(Config,sizeof(*Config)); CR_MDSS_PLATFORM_WITNESS Witness;
  CR_STATUS S=Qualify(P,&Witness); if (CR_ERROR(S)) return S;
  *Config=P->Config; return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformPoll(CR_MDSS_PLATFORM *P, CR_MDSS_PLATFORM_WINDOW W,
                             UINT32 O, UINT32 Mask, UINT32 Expected, UINT32 Timeout) {
  for (UINT32 Elapsed=0; Elapsed<Timeout; Elapsed+=5) {
    if ((P->Io.Read32(P->Io.Context,W,O)&Mask)==Expected) return CR_SUCCESS;
    P->Io.DelayUs(P->Io.Context,5);
  }
  return CR_TIMEOUT;
}
STATIC CR_STATUS EnableClock(CR_MDSS_PLATFORM *P, UINT32 I) {
  P->ClockTouched[I]=TRUE; return Exact(P->Io.Clock(P->Io.Context,P,I,TRUE));
}
STATIC CR_STATUS FenceNative(CR_MDSS_PLATFORM *P) {
  if (P->NativeFenced) return CR_SUCCESS;
  /* A partial claim/resource failure may leave firmware scanout running.
     Cleanup must establish safe access and finish its fence before dropping
     either inherited or newly acquired dependencies. Absolute votes make
     replay of already-held entries idempotent; touched alone is not success. */
  CR_STATUS S;
  for (UINT32 I=0; I<CR_MDSS_PLATFORM_RAILS; ++I) {
    P->RailTouched[I]=TRUE; S=Exact(P->Io.Rail(P->Io.Context,P,I,TRUE)); if (CR_ERROR(S)) return S;
  }
  for (UINT32 I=0; I<CR_MDSS_PLATFORM_PATHS; ++I) {
    UINT64 Average=I==0 ? 1000 : (P->PixelRate*4+999)/1000;
    P->PathTouched[I]=TRUE; S=Exact(P->Io.Bandwidth(P->Io.Context,P,I,Average,I==0?Average:Average*2));
    if (CR_ERROR(S)) return S;
  }
  for (UINT32 I=0; I<2; ++I) { S=EnableClock(P,I); if (CR_ERROR(S)) return S; }
  S=EnableClock(P,9); if (CR_ERROR(S)) return S;
  for (UINT32 I=2; I<9; ++I) { S=EnableClock(P,I); if (CR_ERROR(S)) return S; }
  S=Exact(P->Io.ResourceReady(P->Io.Context,P)); if (CR_ERROR(S)) return S;
  S=CrMdssPlatformPoll(P,CrMdssPlatformDispCc,0x9000,BIT_(31),BIT_(31),5000);
  if (CR_ERROR(S)) return S;
  S=Exact(P->Io.NativeFence(P->Io.Context,P,&P->Witness)); if (CR_ERROR(S)) return S;
  P->NativeFenced=TRUE; return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformReclaimPost(CR_MDSS_PLATFORM *P,VOID *Owner,BOOLEAN *Owned) {
  if(!Owned)return CR_INVALID_PARAMETER;*Owned=FALSE;
  if(!Owner||CR_ERROR(Check(P)))return CR_INVALID_PARAMETER;
  if(P->Owner){*Owned=P->Owner==Owner;return CR_BUSY;}
  if(!P->PostOwned)return CR_NOT_READY;
  CR_MDSS_PLATFORM_WITNESS W;CR_STATUS S=Exact(Qualify(P,&W));if(CR_ERROR(S))return S;
  if(!SameWitness(&W,&P->Witness))return CR_NOT_READY;
  S=Exact(P->Io.ReclaimPost(P->Io.Context,P));if(CR_ERROR(S))return S;
  P->PostOwned=FALSE;P->Owner=Owner;*Owned=TRUE;return CR_SUCCESS;
}
STATIC CR_STATUS Acquire(CR_MDSS_PLATFORM *P, VOID *Owner, UINT64 *Device,
                                UINT32 *Timing, BOOLEAN *Owned,BOOLEAN ResourcesOnly) {
  if (Owned == NULL) return CR_INVALID_PARAMETER; *Owned=FALSE;
  if (Device == NULL || Timing == NULL || Owner == NULL || CR_ERROR(Check(P))) return CR_INVALID_PARAMETER;
  *Device=0; *Timing=0;
  if (P->Owner != NULL) {
    if (P->Owner != Owner) return CR_BUSY;
    if(P->ResourcesOnly!=ResourcesOnly)return CR_BUSY;
    *Owned=TRUE; if (!P->Ready || P->CleanupPending) return CR_NOT_READY;
    CR_MDSS_PLATFORM_WITNESS W; CR_STATUS S=Qualify(P,&W); if (CR_ERROR(S)) return S;
    /* An acquired lease cannot silently follow a new lower-provider incarnation. */
    if (!SameWitness(&W,&P->Witness)) return CR_NOT_READY;
    *Device=P->DeviceAddress; *Timing=P->HostClockTiming; return CR_SUCCESS;
  }
  CR_MDSS_PLATFORM_WITNESS W; CR_STATUS S=Qualify(P,&W); if (CR_ERROR(S)) return S;
  if (P->PostOwned) {
    if(P->ResourcesOnly!=ResourcesOnly||ResourcesOnly)return CR_BUSY;
    /* Resources-only clients must explicitly reclaim POST, stop their full
       pipeline, then release. The base cannot retire their PHY/clock journal. */
    if (!SameWitness(&W,&P->Witness)) return CR_NOT_READY;
    S=CrMdssPlatformReclaimPost(P,Owner,Owned);if(CR_ERROR(S))return S;
    S=CrMdssPlatformRelease(P,Owner); if (CR_ERROR(S)) return S;
    *Owned=FALSE;
    S=Qualify(P,&W); if (CR_ERROR(S)) return S;
  }
  P->Witness=W;
  BOOLEAN Native=FALSE; S=Exact(P->Io.NativeClaim(P->Io.Context,P,&W,&Native));
  P->NativeOwned=Native;
  if (!CR_ERROR(S) && !Native) S=CR_COMPROMISED_DATA;
  if (!Native) return S;
  P->NativeFenced=FALSE;
  P->Owner=Owner;P->ResourcesOnly=ResourcesOnly; *Owned=TRUE; P->CleanupPending=TRUE;
  if (CR_ERROR(S)) return S;
  S=FenceNative(P); if (CR_ERROR(S)) return S;
  if(!ResourcesOnly) {
    S=CrMdssPlatformEnablePhy(P); if (CR_ERROR(S)) return S;
    P->ExternalTouched=TRUE;
    S=Exact(P->Io.ExternalClocks(P->Io.Context,P,P->PixelRate,P->PixelRate*24/4/8,TRUE));
    if (CR_ERROR(S)) return S;
    /* Verify ClockCr's external route only while its exclusive lease is held. */
    if ((P->Io.Read32(P->Io.Context,CrMdssPlatformDispCc,0x8138)&0x371f)!=0x201 ||
        (P->Io.Read32(P->Io.Context,CrMdssPlatformDispCc,0x80c0)&0x371f)!=0x101 ||
        (P->Io.Read32(P->Io.Context,CrMdssPlatformDispCc,0x814c)&15)!=1) return CR_DEVICE_ERROR;
  }
  BOOLEAN Mapped=FALSE;
  S=Exact(P->Io.MapFrameBuffer(P->Io.Context,P,P->Config.FrameBufferPhysical,P->Config.FrameBufferBytes,
                         CrMdssPlatformSids,4,&P->DeviceAddress,&Mapped));
  P->MappingOwned=Mapped;
  if (CR_ERROR(S)) return S;
  if (!Mapped || P->DeviceAddress==0 || P->DeviceAddress > 0xffffffffULL ||
      (P->DeviceAddress & 0xfff) != 0 ||
      P->Config.FrameBufferBytes-1 > 0xffffffffULL-P->DeviceAddress) return CR_UNSUPPORTED;
  P->Ready=TRUE; P->CleanupPending=FALSE; *Device=P->DeviceAddress; *Timing=P->HostClockTiming;
  return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformAcquire(CR_MDSS_PLATFORM *P,VOID *O,UINT64 *D,UINT32 *T,BOOLEAN *Owned) {
  return Acquire(P,O,D,T,Owned,FALSE);
}
CR_STATUS CrMdssPlatformAcquireResources(CR_MDSS_PLATFORM *P,VOID *O,UINT64 *D,BOOLEAN *Owned) {
  UINT32 Timing;return Acquire(P,O,D,&Timing,Owned,TRUE);
}
CR_STATUS CrMdssPlatformPreparePanel(CR_MDSS_PLATFORM *P, VOID *Owner) {
  CR_STATUS S=CheckOwner(P,Owner); if (CR_ERROR(S)) return S;
  if (!P->Ready || P->CleanupPending) return CR_NOT_READY;
  if (P->PanelReady) return CR_SUCCESS;
  CR_MDSS_PLATFORM_WITNESS W; S=Qualify(P,&W); if (CR_ERROR(S)) return S;
  if (!SameWitness(&W,&P->Witness)) return CR_NOT_READY;
  S=CrMdssPlatformBridgePrepare(P); if (CR_ERROR(S)) { P->CleanupPending=TRUE; return S; }
  P->PanelReady=TRUE; return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformUnpreparePanel(CR_MDSS_PLATFORM *P, VOID *Owner) {
  CR_STATUS S=CheckOwner(P,Owner); if (CR_ERROR(S)) return S;
  S=CrMdssPlatformBridgeUnprepare(P); if (CR_ERROR(S)) { P->CleanupPending=TRUE; return S; }
  P->PanelReady=FALSE; return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformRelease(CR_MDSS_PLATFORM *P, VOID *Owner) {
  CR_STATUS S=CheckOwner(P,Owner); if (CR_ERROR(S)) return S;
  P->CleanupPending=TRUE; P->Ready=FALSE;
  if (P->NativeOwned && !P->NativeFenced) {
    S=FenceNative(P); if (CR_ERROR(S)) return S;
  }
  S=CrMdssPlatformUnpreparePanel(P,Owner); if (CR_ERROR(S)) return S;
  if (P->MappingOwned) {
    S=Exact(P->Io.UnmapFrameBuffer(P->Io.Context,P)); if (CR_ERROR(S)) return S;
    P->MappingOwned=FALSE; P->DeviceAddress=0;
  }
  P->DeviceAddress=0;
  if (P->ExternalTouched) {
    S=Exact(P->Io.ExternalClocks(P->Io.Context,P,P->PixelRate,P->PixelRate*24/4/8,FALSE));
    if (CR_ERROR(S)) return S; P->ExternalTouched=FALSE;
  }
  if (P->PhyTouched) {
    /* No raw PHY access after a provider D0 loss. Retain all access/domain
       votes until the same incarnation can qualify them for the retry. */
    S=Exact(P->Io.ResourceReady(P->Io.Context,P)); if (CR_ERROR(S)) return S;
    S=CrMdssPlatformDisablePhy(P); if (CR_ERROR(S)) return S;
  }
  for (UINT32 I=9; I-- > 2;) if (P->ClockTouched[I]) {
    S=Exact(P->Io.Clock(P->Io.Context,P,I,FALSE)); if (CR_ERROR(S)) return S; P->ClockTouched[I]=FALSE;
  }
  if (P->ClockTouched[9]) {
    S=Exact(P->Io.Clock(P->Io.Context,P,9,FALSE)); if (CR_ERROR(S)) return S; P->ClockTouched[9]=FALSE;
  }
  for (UINT32 I=2; I-- > 0;) if (P->ClockTouched[I]) {
    S=Exact(P->Io.Clock(P->Io.Context,P,I,FALSE)); if (CR_ERROR(S)) return S; P->ClockTouched[I]=FALSE;
  }
  for (UINT32 I=CR_MDSS_PLATFORM_PATHS; I-- > 0;) if (P->PathTouched[I]) {
    S=Exact(P->Io.Bandwidth(P->Io.Context,P,I,0,0)); if (CR_ERROR(S)) return S; P->PathTouched[I]=FALSE;
  }
  for (UINT32 I=CR_MDSS_PLATFORM_RAILS; I-- > 0;) if (P->RailTouched[I]) {
    S=Exact(P->Io.Rail(P->Io.Context,P,I,FALSE)); if (CR_ERROR(S)) return S; P->RailTouched[I]=FALSE;
  }
  if (P->NativeOwned) {
    S=Exact(P->Io.NativeRelease(P->Io.Context,P)); if (CR_ERROR(S)) return S;
    P->NativeOwned=FALSE; P->NativeFenced=FALSE;
  }
  P->Owner=NULL; P->CleanupPending=FALSE; P->HostClockTiming=0;P->ResourcesOnly=FALSE; Zero(&P->Witness,sizeof(P->Witness));
  return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformTransferToPost(CR_MDSS_PLATFORM *P, VOID *Owner) {
  CR_STATUS S=CheckOwner(P,Owner); if (CR_ERROR(S)) return S;
  if (!P->Ready || !P->PanelReady || P->CleanupPending || !P->MappingOwned || !P->NativeOwned || !P->NativeFenced)
    return CR_NOT_READY;
  CR_MDSS_PLATFORM_WITNESS W; S=Qualify(P,&W); if (CR_ERROR(S)) return S;
  if (!SameWitness(&W,&P->Witness)) return CR_NOT_READY;
  S=Exact(P->Io.CommitPost(P->Io.Context,P,&P->Config,P->DeviceAddress)); if (CR_ERROR(S)) return S;
  P->PostOwned=TRUE; P->Owner=NULL; return CR_SUCCESS;
}
CR_STATUS CrMdssPlatformUninit(CR_MDSS_PLATFORM *P) {
  if (CR_ERROR(Check(P))) return CR_INVALID_PARAMETER;
  if (P->Owner != NULL || P->PostOwned || P->NativeOwned || P->CleanupPending) return CR_BUSY;
  Zero(P,sizeof(*P)); return CR_SUCCESS;
}
