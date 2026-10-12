/** Native HDK8450 Type-C DisplayPort SST GOP with Shell hot-plug.
    SPDX-License-Identifier: GPL-2.0-only */
#include "DpLink.h"
#include "DpGop.h"
#include "DpReservation.h"
#include <Guid/EventGroup.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFIClockCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFISmmuCrProtocol.h>
#include <Protocol/IoMmu.h>
#include <Protocol/EFIMdssHandoffProtocol.h>

STATIC EFI_USB_TYPEC_CR_PROTOCOL *mTypeC;
STATIC EFI_USB_DP_PHY_CR_PROTOCOL *mPhy;
STATIC EFI_CLOCK_CR_PROTOCOL *mClock;
STATIC EFI_INTERCONNECT_CR_PROTOCOL *mIcb;
STATIC EFI_SMMU_CR_PROTOCOL *mSmmu;
STATIC EDKII_IOMMU_PROTOCOL *mIommu;
STATIC EFI_MDSS_HANDOFF_PROTOCOL *mHandoff;
STATIC BOOLEAN mHandoffRegistered,mFrozen,mHaveTiming;
STATIC EFI_STATUS mFreezeStatus,mRetainedError;
STATIC CR_DP_TIMING mTiming;
STATIC EFI_PHYSICAL_ADDRESS mMappedIova;
STATIC UINT64 mMappedBytes;
STATIC UINT32 mSourceRate,mInterfaceDivider,mPixelM,mPixelN;
STATIC EFI_EVENT mTimer, mExit;
STATIC EFI_USB_TYPEC_STATE mRoute;
STATIC VOID *mMapping;
#ifndef MDEPKG_NDEBUG
STATIC UINT32 mLiveTicks;
#endif
STATIC BOOLEAN mConnectPending,mConnecting;
STATIC BOOLEAN mConsoleConnected;
STATIC UINT32 mConnectAttempts,mConnectRetryTicks;
STATIC BOOLEAN mPhyOwned, mAuxReady, mScanout, mActive, mBusy, mExiting;
STATIC INTERCONNECT_PATH_HANDLE mPaths[2];
STATIC UINT64 mPathAverage[2],mPathPeak[2];
STATIC UINT32 mRetryTicks, mCleanupRetryTicks;
STATIC EFI_USB_TYPEC_STATE mRetryRoute;
STATIC BOOLEAN mHaveRetryRoute;
typedef struct { CONST CHAR8 *Controller, *Name; UINT32 Rate; BOOLEAN Owned; } DP_CLOCK;
STATIC DP_CLOCK mClocks[] = {
  {"gcc", "gcc_disp_ahb_clk", 0, FALSE},
  {"gcc", "gcc_disp_xo_clk", 0, FALSE},
  {"dispcc", "disp_cc_mdss_ahb_clk", 19200000, FALSE},
  {"dispcc", "disp_cc_mdss_mdp_clk", 200000000, FALSE},
  {"gcc", "gcc_disp_hf_axi_clk", 0, FALSE},
  {"gcc", "gcc_disp_sf_axi_clk", 0, FALSE},
  {"dispcc", "disp_cc_mdss_mdp_lut_clk", 200000000, FALSE},
  {"dispcc", "disp_cc_mdss_vsync_clk", 19200000, FALSE},
  {"dispcc", "disp_cc_mdss_dptx0_aux_clk", 19200000, FALSE},
  {"dispcc", "disp_cc_mdss_dptx0_link_clk", 0, FALSE},
  {"dispcc", "disp_cc_mdss_dptx0_link_intf_clk", 0, FALSE},
  {"dispcc", "disp_cc_mdss_dptx0_pixel0_clk", 0, FALSE}
};
#define LINK_CLOCK 9U

STATIC EFI_STATUS ClocksOff(UINTN First)
{
  UINTN I;
  EFI_STATUS Status;
  for (I = ARRAY_SIZE(mClocks); I-- > First;) if (mClocks[I].Owned) {
    Status = mClock->SetClock(mClock, mClocks[I].Controller, mClocks[I].Name, 0, FALSE);
    if (Status!=EFI_SUCCESS) return Status;
    mClocks[I].Owned = FALSE;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS ClockOn(UINTN I)
{
  EFI_STATUS Status = CrAcquireClock(mClock, mClocks[I].Controller, mClocks[I].Name,
                                      mClocks[I].Rate, &mClocks[I].Owned);
  return Status;
}
STATIC EFI_STATUS StopHardware(VOID)
{
  EFI_STATUS Status;
  mActive = FALSE;
  if (mScanout) {
    Status = CrDpStopScanout();
    if (Status!=EFI_SUCCESS) return Status;
    mScanout = FALSE;
  }
  if (mAuxReady) {
    MmioWrite32(CR_DP_LINK + 4, 0);
    MmioAnd32(CR_DP_LINK, (UINT32)~BIT0);
    MemoryFence();
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Stop(VOID)
{
  UINTN I;
  EFI_STATUS Status = StopHardware();
  if (Status!=EFI_SUCCESS) {mRetainedError=Status;return Status;}
  if (mMapping != NULL) {
    Status = mIommu->Unmap(mIommu, mMapping);
    if (Status!=EFI_SUCCESS) {mRetainedError=Status;return Status;}
    mMapping = NULL;
    mMappedIova=mMappedBytes=0;
  }
  if (mAuxReady) { CrDpAuxDisable(); mAuxReady = FALSE; }
  Status = ClocksOff(0);
  if (Status!=EFI_SUCCESS) {mRetainedError=Status;return Status;}
  if (mPhyOwned) {
    Status = mPhy->Release(mPhy, UsbDpPhyDp);
    if (Status!=EFI_SUCCESS) {mRetainedError=Status;return Status;}
    mPhyOwned = FALSE;
  }
  for (I = 2; I-- > 0;) if (mPaths[I] != INTERCONNECT_INVALID_PATH) {
    Status = mIcb->ReleasePath(mIcb, mPaths[I]);
    if (Status!=EFI_SUCCESS) {mRetainedError=Status;return Status;}
    mPaths[I] = INTERCONNECT_INVALID_PATH;
    mPathAverage[I]=mPathPeak[I]=0;
  }
  mRetainedError=EFI_SUCCESS;return EFI_SUCCESS;
}
STATIC EFI_STATUS EFIAPI FreezeParticipant(VOID *Context){
  if(Context!=&mRoute)return EFI_INVALID_PARAMETER;
  mExiting=TRUE;CrDpGopExit();
  if(mBusy||mConnecting||!CrDpGopFrozen())return EFI_NOT_READY;
  if(mFrozen)return EFI_SUCCESS;
  mFreezeStatus=StopHardware();mFrozen=TRUE;
  DEBUG((mFreezeStatus==EFI_SUCCESS?DEBUG_INIT:DEBUG_ERROR,"MDSSDpCr: immutable handoff DMA stop: %r\n",mFreezeStatus));
  /* Retired software is immutable even if hardware stop failed. */
  return EFI_SUCCESS;
}
STATIC VOID HandoffName(CHAR8 *Out,UINTN Capacity,CONST CHAR8 *Name){
  UINTN I=0;while(Name[I]&&I+1<Capacity){Out[I]=Name[I];++I;}
}
STATIC EFI_STATUS EFIAPI SnapshotParticipant(VOID *Context,CR_MDSS_HANDOFF_PARTICIPANT *P,VOID **Map){
  if(Context!=&mRoute||!P||!Map)return EFI_INVALID_PARAMETER;
  if(!mFrozen||mBusy||mConnecting||!CrDpGopFrozen())return EFI_NOT_READY;
  ZeroMem(P,sizeof(*P));*Map=mMapping;P->Owner=CR_MDSS_NATIVE_DP;
  CR_MDSS_NATIVE_ROUTE *R=&P->Route;R->Owner=CR_MDSS_NATIVE_DP;
  EFI_PHYSICAL_ADDRESS Fb=0;UINT64 Bytes=0;BOOLEAN Prepared=FALSE;
  BOOLEAN Pages=CrDpGopAllocation(&Fb,&Bytes,&Prepared),Resources=Pages||mMapping||mPhyOwned;
  for(UINTN I=0;I<ARRAY_SIZE(mClocks);++I)if(mClocks[I].Owned){
    Resources=TRUE;CR_MDSS_NATIVE_CLOCK *C=&P->Clocks[P->ClockCount++];
    C->Owner=CR_MDSS_NATIVE_DP;C->Flags=CR_MDSS_NATIVE_OWNED;C->RateHz=mClocks[I].Rate;
    HandoffName(C->Controller,sizeof(C->Controller),mClocks[I].Controller);HandoffName(C->Name,sizeof(C->Name),mClocks[I].Name);
  }
  for(UINTN I=0;I<ARRAY_SIZE(mPaths);++I)if(mPaths[I]!=INTERCONNECT_INVALID_PATH){
    Resources=TRUE;CR_MDSS_NATIVE_PATH *Q=&P->Paths[P->PathCount++];
    Q->Owner=CR_MDSS_NATIVE_DP;Q->Flags=CR_MDSS_NATIVE_OWNED;Q->Source=I?0x4002:0x4000;Q->Destination=Q->Source+1;
    Q->Average=mPathAverage[I];Q->Peak=mPathPeak[I];HandoffName(Q->Provider,sizeof(Q->Provider),"qcom,sm8450-mdp");
  }
  if(!Resources){R->State=CR_MDSS_NATIVE_ABSENT;return EFI_SUCCESS;}
  BOOLEAN Uncertain=mFreezeStatus!=EFI_SUCCESS||mRetainedError!=EFI_SUCCESS||mScanout||!Pages||!Prepared||!mHaveTiming||
    (mMapping&&(!mMappedIova||mMappedBytes!=Bytes));
  R->State=Uncertain?CR_MDSS_NATIVE_UNCERTAIN:CR_MDSS_NATIVE_STOPPED;
  R->Flags=CR_MDSS_NATIVE_OWNED|(Uncertain?CR_MDSS_NATIVE_RETAINED_ERROR:0);
  R->Dpu=0x0AE01000;R->DpuBytes=0x8F000;
  R->InterfaceBase=R->Dpu+0x34000;R->InterfaceBytes=0x400;
  R->PipeBase=R->Dpu+0x26000;R->PipeBytes=0x400;
  R->CtlBase=R->Dpu+0x16000;R->CtlBytes=0x400;
  R->MixerBase=R->Dpu+0x46000;R->MixerBytes=0x400;
  R->LinkBase=CR_DP_LINK;R->LinkBytes=0xC00;
  R->PhyBase=0x088EAA00;R->PhyBytes=0x200;R->PllBase=0x088EA000;R->PllBytes=0x200;
  R->FrameBuffer=Fb;R->FrameBufferBytes=Bytes;R->DeviceAddress=mMapping?mMappedIova:0;
  if(Pages&&Prepared&&mHaveTiming){R->Mode=(CR_MDSS_NATIVE_MODE){CR_DP_GOP_WIDTH,CR_DP_GOP_HEIGHT,CR_DP_GOP_STRIDE,
    CR_MDSS_NATIVE_FORMAT_BGRX8888,mTiming.HFront,mTiming.HSync,mTiming.HBack,mTiming.VFront,mTiming.VSync,mTiming.VBack,mTiming.PixelHz};}
  /* Half-divider values match the exact ConfigureExternalSource calls;
     InterfaceDivider is the observed read-only integer divider. */
  R->SourceRate=mSourceRate;R->SourceSelect=1;R->SourceDivider=2;R->InterfaceDivider=mInterfaceDivider;
  R->PixelSource=2;R->PixelDivider=2;R->PixelM=mPixelM;R->PixelN=mPixelN;R->PixelD=mPixelN;
  return EFI_SUCCESS;
}
STATIC EFI_STATUS LinkClocks(CONST CR_DP_TIMING *T, UINT32 Rate)
{
  UINT32 Input = Rate == 1620 ? 810000000 : 1350000000;
  UINT32 M = T->PixelHz, N = Input, A, B, G;
  EFI_STATUS Status;
  UINTN I;
  A = M; B = N;
  while (B != 0) { G = A % B; A = B; B = G; }
  M /= A; N /= A;
  if (N > 0xFFFF) return EFI_UNSUPPORTED;
  Status = ClocksOff(LINK_CLOCK);
  if (EFI_ERROR(Status)) return Status;
  Status = mClock->ConfigureExternalSource(mClock, "dispcc", mClocks[9].Name,
                                           Rate * 100000, 1, 2, 0, 0, 0);
  if (EFI_ERROR(Status)) return Status;
  Status = mClock->ConfigureExternalSource(mClock, "dispcc", mClocks[11].Name,
                                           T->PixelHz, 2, 2, M, N, N);
  if (EFI_ERROR(Status)) return Status;
  /* Linux exposes this divisor as read-only. Preserve the boot-programmed
     value while validating it through the clock provider. */
  UINT32 Divider=(MmioRead32(0x0AF00000+0x81B4)&15)+1;
  Status = mClock->SetDivider(mClock, "dispcc", mClocks[10].Name,Divider);
  if (EFI_ERROR(Status)) return Status;
  mSourceRate=Rate*100000;mPixelM=M;mPixelN=N;mInterfaceDivider=Divider;
  for (I = LINK_CLOCK; I < ARRAY_SIZE(mClocks); ++I) {
    Status = ClockOn(I);
    if (EFI_ERROR(Status)) return Status;
  }
  return EFI_SUCCESS;
}
STATIC EFI_STATUS Start(CONST EFI_USB_TYPEC_STATE *State)
{
  STATIC CONST UINT16 Sids[] = {0x2800,0x2802,0x2C00,0x2C02};
  STATIC CONST UINT32 Rates[] = {1620,2700,5400,8100};
  UINT8 Dpcd[16], Edid[512], Value;
  CR_DP_TIMING Timing;
  CR_DP_TU Tu;
  UINT32 Width, Height, Stride, Rate, MaxRate;
  UINT8 Lanes, MaxLanes;
  UINTN I, Bytes, Blocks;
  EFI_PHYSICAL_ADDRESS Fb, Iova;
  EFI_STATUS Status;
  Width=CR_DP_GOP_WIDTH;Height=CR_DP_GOP_HEIGHT;Stride=CR_DP_GOP_STRIDE;
  for (I = 0; I < 2; ++I) {
    Status = mIcb->AcquirePath(mIcb, "qcom,sm8450-mdp", I == 0 ? 0x4000 : 0x4002,
                              I == 0 ? 0x4001 : 0x4003, &mPaths[I]);
    if (EFI_ERROR(Status)) return Status;
    /* Retain the exact attempted demand before a fallible backend mutation. */
    mPathAverage[I]=I?0:600000;mPathPeak[I]=I?19200:2500000;
    Status = mIcb->SetBandwidth(mIcb, mPaths[I], mPathAverage[I],mPathPeak[I]);
    if (EFI_ERROR(Status)) return Status;
  }
  for (I = 0; I < LINK_CLOCK; ++I) {
    Status = ClockOn(I);
    if (EFI_ERROR(Status)) return Status;
  }
  Status = mPhy->Acquire(mPhy, UsbDpPhyDp, State, FALSE, &mPhyOwned);
  if (EFI_ERROR(Status)) return Status;
  mRoute = *State;
  MmioOr32(CR_DP_AHB + 0x10, BIT0);
  MicroSecondDelay(1000);
  MmioAnd32(CR_DP_AHB + 0x10, (UINT32)~BIT0);
  CrDpAuxEnable(); mAuxReady = TRUE;
  Status = CrDpAuxTransfer(TRUE, TRUE, 0, Dpcd, 16);
  if (EFI_ERROR(Status)) return Status;
  /* DP 1.3+ may publish receiver caps at 0x2200. */
  if ((Dpcd[14] & BIT7) != 0) {
    Status = CrDpAuxTransfer(TRUE, TRUE, 0x2200, Dpcd, 16);
    if (EFI_ERROR(Status)) return Status;
  }
  MaxRate = Dpcd[1] * 270U; MaxLanes = Dpcd[2] & 31;
  if (Dpcd[0] < 0x11 || (MaxLanes != 1 && MaxLanes != 2 && MaxLanes != 4) ||
      (MaxRate != 1620 && MaxRate != 2700 && MaxRate != 5400 && MaxRate != 8100))
    return EFI_UNSUPPORTED;
  if (MaxLanes > State->DpLanes) MaxLanes = State->DpLanes;
  if (Dpcd[0] >= 0x11) {
    Value = 1;
    Status = CrDpAuxTransfer(TRUE, FALSE, 0x600, &Value, 1);
    if (EFI_ERROR(Status)) return Status;
    MicroSecondDelay(1000);
  }
  Status = CrDpReadEdid(0, Edid);
  if (EFI_ERROR(Status)) return Status;
  Blocks = MIN((UINTN)Edid[126] + 1, ARRAY_SIZE(Edid) / 128);
  for (I = 1; I < Blocks; ++I) {
    Status = CrDpReadEdid((UINT8)I, Edid + I * 128);
    if (EFI_ERROR(Status)) return Status;
  }
  Status = CrDpFindMirrorTiming(Edid, Blocks * 128, Width, Height, &Timing);
  if (EFI_ERROR(Status)) return Status;
  DEBUG((DEBUG_WARN, "MDSSDpCr: receiver DPCD=%02x rate=%u lanes=%u EDID-blocks=%u GOP=%ux%u pixel=%u\n",
         Dpcd[0], MaxRate, MaxLanes, (UINT32)Blocks, Width, Height, Timing.PixelHz));
  /* Start with the lowest sufficient rate and full negotiated lane count.
     TU buffering must fit the actual sink timing; try other lane/rate pairs
     on training failure without approximating sink capabilities. */
  Status = EFI_UNSUPPORTED;
  for (Lanes = MaxLanes; Lanes != 0; Lanes /= 2) {
    for (I = 0; I < ARRAY_SIZE(Rates); ++I) {
      Rate = Rates[I];
      if (Rate > MaxRate || EFI_ERROR(CrDpCalculateTu(&Timing, Rate, Lanes, &Tu))) continue;
      Status = ClocksOff(LINK_CLOCK);
      if (EFI_ERROR(Status)) return Status;
      Status = mPhy->PowerDp(mPhy, Rate, Lanes);
      if (EFI_ERROR(Status)) continue;
      Status = LinkClocks(&Timing, Rate);
      if (EFI_ERROR(Status)) return Status;
      Status = CrDpTrain(mPhy, Dpcd, Rate, Lanes);
      if (!EFI_ERROR(Status)) goto Trained;
    }
  }
  return Status;
Trained:
  for (I = 0; I < ARRAY_SIZE(Sids); ++I) {
    Status = mSmmu->Attach(mSmmu, 0, 0xFFFF, Sids[I]);
    if (EFI_ERROR(Status)) return Status;
  }
  Status=CrDpGopPrepare();if(Status!=EFI_SUCCESS)return Status;
  Status=CrDpGopGetFramebuffer(&Fb,&Width,&Height,&Stride);if(Status!=EFI_SUCCESS)return Status;
  Bytes=(UINTN)Stride*Height;
#ifndef MDEPKG_NDEBUG
  mLiveTicks=0;
#endif
  Iova=0;
  Status = mIommu->Map(mIommu, EdkiiIoMmuOperationBusMasterRead64,
                       (VOID *)(UINTN)Fb, &Bytes, &Iova, &mMapping);
  if(mMapping){mMappedIova=Iova;mMappedBytes=Bytes;}
  if (EFI_ERROR(Status)) return Status;
  if (Bytes != (UINTN)Stride * Height || Iova > MAX_UINT32 ||
      Iova + Bytes - 1 > MAX_UINT32) return EFI_BAD_BUFFER_SIZE;
  for (I = 0; I < ARRAY_SIZE(Sids); ++I) {
    Status = mIommu->SetAttributeById(mIommu, 0x15000000, Sids[I], mMapping, EDKII_IOMMU_ACCESS_READ);
    if (EFI_ERROR(Status)) return Status;
  }
  CrDpConfigureStream(&Timing, Rate, &Tu);
  mTiming=Timing;mHaveTiming=TRUE;
  mScanout = TRUE;
  CrDpStartScanout(&Timing, Iova, Stride);
  MmioWrite32(CR_DP_LINK + 4, BIT7);
  Status = CrDpWait(CR_DP_LINK + 0x40, BIT0, BIT0, 100000);
  if (!EFI_ERROR(Status)) Status = CrDpWaitScanout();
  if (EFI_ERROR(Status)) return Status;
  mActive = TRUE;
  DEBUG((DEBUG_WARN,"MDSSDpCr: independent GOP active %ux%u rate=%u lanes=%u TU=%u/%u CPU=%lx IOVA=%lx; no source mirror timer\n",
    Width,Height,Rate,Lanes,Tu.Valid,Tu.Size,Fb,Iova));
  BOOLEAN NewPublication=FALSE;
  Status=CrDpGopPublish(&NewPublication);
  if(Status==EFI_SUCCESS&&!mConsoleConnected){
    mConnectPending=TRUE;mConnectAttempts=mConnectRetryTicks=0;
  }
  return Status;
}


STATIC BOOLEAN SameTopology(CONST EFI_USB_TYPEC_STATE *A, CONST EFI_USB_TYPEC_STATE *B)
{
  return A->Orientation == B->Orientation && A->DpActive == B->DpActive &&
         A->DpLanes == B->DpLanes && A->PinAssignment == B->PinAssignment && A->Usb3 == B->Usb3;
}
#ifndef MDEPKG_NDEBUG
STATIC VOID ObserveTestPixels(VOID)
{
  UINT32 Samples[8], I, B;
  if (mLiveTicks < 100) ++mLiveTicks;
  else return;
  if (mLiveTicks != 10 && mLiveTicks != 30 && mLiveTicks != 100) return;
  EFI_PHYSICAL_ADDRESS Fb;UINT32 Width,Height,Stride;
  if(CrDpGopGetFramebuffer(&Fb,&Width,&Height,&Stride)!=EFI_SUCCESS)return;
  /* Read only our retained allocation, never the source GOP. Centre samples
     distinguish all eight bands without changing the CPU/DMA publication. */
  for (I = 0; I < 8; ++I) {
    UINTN Offset = (UINTN)(Height / 2) * Stride +
      (UINTN)(((UINT64)Width * (2 * I + 1)) / 16) * 4;
    CONST UINT8 *P = (CONST UINT8 *)(UINTN)Fb + Offset;
    Samples[I] = 0;
    for (B = 0; B < 4; ++B) Samples[I] |= (UINT32)P[B] << (8 * B);
  }
  DEBUG((DEBUG_WARN, "MDSSDpCr: DEBUG retained %a at %ums CPU samples=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
    "DP GOP", mLiveTicks * 100, Samples[0], Samples[1], Samples[2], Samples[3],
    Samples[4], Samples[5], Samples[6], Samples[7]));
  CrDpSnapshotScanout();
  CrDpObserveLink();
}
#endif
STATIC VOID EFIAPI Monitor(EFI_EVENT Event, VOID *Context)
{
  EFI_USB_TYPEC_STATE State;
  EFI_STATUS Status;
  BOOLEAN Wanted;
  (VOID)Event; (VOID)Context;
  if (mExiting || mBusy || mConnecting) return;
  mBusy = TRUE;
  /* UCSI owns a Shell unload path. Refresh before using its callbacks. */
  mTypeC = NULL;
  Status = gBS->LocateProtocol(&gEfiUsbTypeCCrProtocolGuid, NULL, (VOID **)&mTypeC);
  if (!EFI_ERROR(Status) && mTypeC != NULL &&
      mTypeC->Revision >= EFI_USB_TYPEC_CR_PROTOCOL_REVISION && mTypeC->GetState != NULL)
    Status = mTypeC->GetState(mTypeC, &State);
  else Status = EFI_NOT_READY;
  Wanted = !EFI_ERROR(Status) && !EFI_ERROR(State.LastError) && State.PdReady &&
           State.Connected && State.DpActive && State.Hpd;
  if (mActive && Wanted && SameTopology(&State, &mRoute)) {
#ifndef MDEPKG_NDEBUG
    ObserveTestPixels();
#endif
    if (State.HpdIrqSequence == mRoute.HpdIrqSequence) goto Out;
    Status = CrDpServiceIrq();
    /* A short HPD pulse need not disturb an equalized stream. Linux checks
       actual receiver lane status for maintenance; retain the current pixels
       and mapping after a successful service/status observation. */
    if (!EFI_ERROR(Status)) Status = CrDpObserveLink();
    if (Status == EFI_SUCCESS) {
      mRoute.HpdIrqSequence = State.HpdIrqSequence;
      goto Out;
    }
    if (EFI_ERROR(Status)) DEBUG((DEBUG_ERROR, "MDSSDpCr: HPD service: %r\n", Status));
  }
  if (mCleanupRetryTicks != 0) { --mCleanupRetryTicks; goto Out; }
  Status = Stop();
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "MDSSDpCr: cleanup retained: %r\n", Status));
    mCleanupRetryTicks = 10; /* retain resources; retry after one second */
    goto Out;
  }
  if (!Wanted) { mRetryTicks = 0; mHaveRetryRoute = FALSE; goto Out; }
  if (!mHaveRetryRoute || !SameTopology(&State, &mRetryRoute)) {
    mRetryRoute = State; mHaveRetryRoute = TRUE; mRetryTicks = 0;
  }
  if (mRetryTicks != 0) { --mRetryTicks; goto Out; }
  Status = Start(&State);
  if (EFI_ERROR(Status)) {
    EFI_STATUS Cleanup = Stop();
    DEBUG((DEBUG_ERROR, "MDSSDpCr: hot-plug start: %r cleanup: %r\n", Status, Cleanup));
    if (EFI_ERROR(Cleanup)) mCleanupRetryTicks = 10;
    mRetryTicks = 50; /* bounded retry every five seconds while connected */
  }
Out:
  if(mConnectPending&&mActive&&mConnectRetryTicks)--mConnectRetryTicks;
  mConnecting=mConnectPending&&mActive&&!mExiting&&!mConnectRetryTicks&&mConnectAttempts<6;
  mBusy=FALSE;
  if(mConnecting){
    ++mConnectAttempts;
    Status=CrDpGopConnect();
    DEBUG((EFI_ERROR(Status)?DEBUG_WARN:DEBUG_INIT,"MDSSDpCr: DP GOP console connect: %r\n",Status));
    if(Status==EFI_SUCCESS){mConsoleConnected=TRUE;mConnectPending=FALSE;}
    else if(mConnectAttempts<6)mConnectRetryTicks=10;
    else mConnectPending=FALSE;
    mConnecting=FALSE;
  }
}
STATIC VOID EFIAPI Exit(EFI_EVENT Event, VOID *Context)
{
  EFI_STATUS Status;
  (VOID)Event; (VOID)Context;
  Status=FreezeParticipant(&mRoute);
  if (mTimer != NULL) gBS->SetTimer(mTimer, TimerCancel, 0);
  /* Retain published GOP pages/mode pointers. Stop owned DMA before the
     ordinary SMMU mapping is revoked; final cross-driver EBS evidence is
     coordinated separately, so a failed stop keeps all owned resources. */
  DEBUG((Status==EFI_SUCCESS&&mFreezeStatus==EFI_SUCCESS?DEBUG_INIT:DEBUG_ERROR,
    "MDSSDpCr: exit software freeze: %r scanout stop: %r\n",Status,mFreezeStatus));
}
EFI_STATUS EFIAPI MDSSDpCrDxeUnload(EFI_HANDLE ImageHandle)
{
  EFI_STATUS Status;
  (VOID)ImageHandle;
  if (mBusy || mConnecting || mExiting || CrDpGopPublished()) return EFI_ACCESS_DENIED;
  mBusy=TRUE;
  if (mTimer != NULL) gBS->SetTimer(mTimer, TimerCancel, 0);
  Status = Stop();
  if (Status!=EFI_SUCCESS) goto Done;
  Status=CrDpGopRelease();if(Status!=EFI_SUCCESS)goto Done;
  if(mHandoffRegistered){
    Status=mHandoff->SetParticipant(mHandoff,CR_MDSS_NATIVE_DP,&mRoute,NULL,NULL);
    if(Status!=EFI_SUCCESS)goto Done;mHandoffRegistered=FALSE;
  }
  if (mTimer != NULL) { gBS->CloseEvent(mTimer); mTimer = NULL; }
  if (mExit != NULL) { gBS->CloseEvent(mExit); mExit = NULL; }
  Status=EFI_SUCCESS;
Done:
  mBusy=FALSE;return Status;
}
EFI_STATUS EFIAPI MDSSDpCrDxeEntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  (VOID)SystemTable;
  if (mTimer != NULL || mExit != NULL || mHandoffRegistered) return EFI_ALREADY_STARTED;

  mPaths[0] = mPaths[1] = INTERCONNECT_INVALID_PATH;
  ZeroMem(mPathAverage,sizeof(mPathAverage));ZeroMem(mPathPeak,sizeof(mPathPeak));
#define LOCATE(Guid, Pointer) \
  Status = gBS->LocateProtocol(&(Guid), NULL, (VOID **)&(Pointer)); \
  if (EFI_ERROR(Status)) return Status
  LOCATE(gEfiUsbTypeCCrProtocolGuid, mTypeC);
  LOCATE(gEfiUsbDpPhyCrProtocolGuid, mPhy);
  LOCATE(gEfiClockCrProtocolGuid, mClock);
  LOCATE(gEfiInterconnectCrProtocolGuid, mIcb);
  LOCATE(gEfiSmmuCrProtocolGuid, mSmmu);
  LOCATE(gEdkiiIoMmuProtocolGuid, mIommu);
  LOCATE(gEfiMdssHandoffProtocolGuid,mHandoff);
#undef LOCATE
  if (mTypeC == NULL || mPhy == NULL || mClock == NULL || mIcb == NULL || mSmmu == NULL || mIommu == NULL ||
      mTypeC->GetState == NULL || mPhy->Acquire == NULL || mPhy->Release == NULL ||
      mPhy->PowerDp == NULL || mPhy->SetDpLevels == NULL || mClock->SetClock == NULL ||
      mClock->ConfigureExternalSource == NULL || mClock->SetDivider == NULL || mIcb->AcquirePath == NULL ||
      mIcb->SetBandwidth == NULL || mIcb->ReleasePath == NULL || mSmmu->Attach == NULL ||
      mIommu->Revision < EDKII_IOMMU_PROTOCOL_REVISION || mIommu->Map == NULL ||
      mIommu->Unmap == NULL || mIommu->SetAttributeById == NULL) return EFI_UNSUPPORTED;
  if(!mHandoff||mHandoff->Revision<EFI_MDSS_HANDOFF_PROTOCOL_REVISION||!mHandoff->SetParticipant)return EFI_UNSUPPORTED;
  if(gBS->InstallMultipleProtocolInterfaces==NULL||
      gBS->ConnectController==NULL||gBS->HandleProtocol==NULL||gBS->RaiseTPL==NULL||gBS->RestoreTPL==NULL)return EFI_UNSUPPORTED;
  Status=CrDpReservationInit(ImageHandle);if(Status!=EFI_SUCCESS)return Status;
  Status=mHandoff->SetParticipant(mHandoff,CR_MDSS_NATIVE_DP,&mRoute,FreezeParticipant,SnapshotParticipant);
  if(Status!=EFI_SUCCESS)return Status;mHandoffRegistered=TRUE;
  mBusy = TRUE;
  Status = gBS->CreateEventEx(EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Exit, NULL,
                              &gEfiEventExitBootServicesGuid, &mExit);
  if (!EFI_ERROR(Status)) Status = gBS->CreateEvent(EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, Monitor, NULL, &mTimer);
  if (!EFI_ERROR(Status)) Status = gBS->SetTimer(mTimer, TimerPeriodic, 1000000);
  mBusy = FALSE;
  if (EFI_ERROR(Status)) {
    EFI_STATUS Cleanup = MDSSDpCrDxeUnload(ImageHandle);
    if (EFI_ERROR(Cleanup)) {
      DEBUG((DEBUG_ERROR, "MDSSDpCr: monitor setup: %r cleanup retained: %r\n", Status, Cleanup));
      return EFI_SUCCESS;
    }
    return Status;
  }
  DEBUG((DEBUG_INIT, "MDSSDpCr: Type-C DP independent GOP hot-plug monitor ready\n"));
  return EFI_SUCCESS;
}
