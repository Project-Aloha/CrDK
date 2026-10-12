/** Native SM8450 DisplayPort transport. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Uefi.h>
#include <Protocol/EFIUsbDpPhyCrProtocol.h>

#define CR_DP_AHB  0x0AE90000U
#define CR_DP_AUX  0x0AE90200U
#define CR_DP_LINK 0x0AE90400U
#define CR_DP_P0   0x0AE91000U
typedef struct {
  UINT32 PixelHz;
  UINT16 Width, Height, HFront, HSync, HBack, VFront, VSync, VBack;
  BOOLEAN HNegative, VNegative;
} CR_DP_TIMING;
typedef struct {
  UINT8 Size, Valid;
  UINT16 Delay;
} CR_DP_TU;

VOID CrDpAuxEnable(VOID);
VOID CrDpAuxDisable(VOID);
EFI_STATUS CrDpServiceIrq(VOID);
EFI_STATUS CrDpAuxTransfer(BOOLEAN Native, BOOLEAN Read, UINT32 Address,
                          UINT8 *Buffer, UINTN Size);
EFI_STATUS CrDpReadEdid(UINT8 Block, UINT8 *Data);
EFI_STATUS CrDpFindMirrorTiming(CONST UINT8 *Edid, UINTN Size,
                               UINT32 Width, UINT32 Height, CR_DP_TIMING *Timing);
BOOLEAN CrDpLaneStatus(CONST UINT8 *Status, UINT8 Lanes, BOOLEAN Equalized);
EFI_STATUS CrDpCalculateTu(CONST CR_DP_TIMING *Timing, UINT32 Rate, UINT8 Lanes,
                           CR_DP_TU *Tu);
EFI_STATUS CrDpTrain(EFI_USB_DP_PHY_CR_PROTOCOL *Phy, CONST UINT8 *Dpcd,
                    UINT32 Rate, UINT8 Lanes);
VOID CrDpConfigureStream(CONST CR_DP_TIMING *Timing, UINT32 Rate,
                         CONST CR_DP_TU *Tu);
EFI_STATUS CrDpWait(UINTN Address, UINT32 Mask, UINT32 Expected, UINT32 TimeoutUs);
VOID CrDpStartScanout(CONST CR_DP_TIMING *Timing, EFI_PHYSICAL_ADDRESS Iova, UINT32 Stride);
EFI_STATUS CrDpStopScanout(VOID);
EFI_STATUS CrDpWaitScanout(VOID);
/* Pure bounded live observations; caller holds the complete route/clocks.
   A healthy timing status does not certify visible framebuffer pixels. */
VOID CrDpSnapshotScanout(VOID);
EFI_STATUS CrDpObserveLink(VOID);
EFI_STATUS CrDpFillColorBars(VOID *Framebuffer,UINT32 Width,UINT32 Height,UINT32 Stride);
/* Copy compatible B,G,R,X GOP pixels into the retained DP surface. This is
   CPU publication only: the caller keeps the same live SMMU/DMA mapping. */
EFI_STATUS CrDpCopyFramebuffer(VOID *Framebuffer,UINT32 Width,UINT32 Height,
                              UINT32 Stride,CONST VOID *Source,UINT32 SourceStride);
