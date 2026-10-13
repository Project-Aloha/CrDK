/* SM8450 DPU8.1 DSC1.2 engine, RGB PPS and rate control.
 * Adapted from Qualcomm sde_dsc_helper.c and Linux dpu_hw_dsc_1_2.c.
 * Copyright (c) 2012-2021 The Linux Foundation.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>
#define CR_MDSS_DSC_REVISION 1U
#define CR_MDSS_DSC_PPS_BYTES 128U
#define CR_MDSS_DSC_RANGES 15U
/* Explicit full panel PPS picture, not a guessed per-link half. RGB444 supports the qualified
 * Android table tuples 8bpc/8bpp, 10bpc/8bpp and 10bpc/10bpp. BppQ4 is U6.4.
 * Native 422/420 and fractional bpp are rejected until their input path exists. */
typedef struct {
  UINT32 Revision, MinorVersion, ScrVersion, BitsPerComponent, BppQ4;
  UINT32 PictureWidth, PictureHeight, SliceWidth, SliceHeight;
  BOOLEAN BlockPrediction;
} CR_MDSS_DSC_PARAMS;
typedef struct { UINT8 MinQp, MaxQp; INT8 BpgOffset; } CR_MDSS_DSC_RANGE;
typedef struct {
  CR_MDSS_DSC_PARAMS Params;
  UINT32 SliceCount, ChunkBytes, LineBufferDepth, InitialTransmitDelay;
  UINT32 InitialDecodeDelay, InitialScale, ScaleIncrement, ScaleDecrement;
  UINT32 FirstLineBpg, NflBpg, SliceBpg, InitialOffset, FinalOffset;
  UINT32 FlatnessMinQp, FlatnessMaxQp, QuantLimit, RcModelSize;
  UINT8 Thresholds[14];
  CR_MDSS_DSC_RANGE Ranges[CR_MDSS_DSC_RANGES];
} CR_MDSS_DSC_CONFIG;
CR_STATUS CrMdssDscBuild(CONST CR_MDSS_DSC_PARAMS *, CR_MDSS_DSC_CONFIG *);
CR_STATUS CrMdssDscValidate(CONST CR_MDSS_DSC_CONFIG *);
CR_STATUS CrMdssDscPackPps(CONST CR_MDSS_DSC_CONFIG *, UINT8 Identifier,
                           UINT8 *Payload, UINTN Bytes);
/* Caller owns the complete DCE (two hard slices share COMMON_MAIN), its DPU
 * access/domain leases and external serialization. DMA must be stopped before
 * Stop. Ready proves this exact ownership/mapping incarnation before every
 * register access; a successful write is not inferred from a failed callback.
 * Nonzero synchronous statuses cannot certify completed access. */
typedef struct {
  VOID *Context;
  UINTN DpuBytes;
  CR_STATUS (*Ready)(VOID *);
  CR_STATUS (*Read32)(VOID *, UINT32 Offset, UINT32 *Value);
  CR_STATUS (*Write32)(VOID *, UINT32 Offset, UINT32 Value);
  CR_STATUS (*Fence)(VOID *);
} CR_MDSS_DSC_IO;
typedef struct {
  UINT32 Encoder, Pingpong, EncoderCount, ActiveSoftSlices, InitialLines;
  BOOLEAN SplitPanel, Multiplex, Video;
} CR_MDSS_DSC_ROUTE;
typedef struct {
  CR_MDSS_DSC_IO Io;
  CR_MDSS_DSC_CONFIG Config;
  CR_MDSS_DSC_ROUTE Route;
  UINT32 Base, EncoderOffset, ControlOffset;
  BOOLEAN Initialized, Touched, Active, CleanupPending;
} CR_MDSS_DSC;
CR_STATUS CrMdssDscInit(CR_MDSS_DSC *, CONST CR_MDSS_DSC_IO *,
                        CONST CR_MDSS_DSC_CONFIG *, CONST CR_MDSS_DSC_ROUTE *);
CR_STATUS CrMdssDscStart(CR_MDSS_DSC *);
CR_STATUS CrMdssDscStop(CR_MDSS_DSC *);
CR_STATUS CrMdssDscUninit(CR_MDSS_DSC *);
