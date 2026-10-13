/* Qualcomm DP AUX transport adapted from Linux msm/dp/dp_aux.c.
 * Copyright (c) 2012-2020, The Linux Foundation. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_MDSS_DP_AHB_PHYSICAL 0x0ae90000ULL
#define CR_MDSS_DP_AUX_PHYSICAL 0x0ae90200ULL
#define CR_MDSS_DP_VIEW_BYTES 0x200U
#define CR_MDSS_DP_AUX_MAX_BYTES 124U
#define CR_MDSS_DP_AUX_NATIVE_MAX_BYTES 16U
#define CR_MDSS_DP_EDID_BYTES 128U

typedef enum { CrMdssDpAhb, CrMdssDpAux, CrMdssDpWindowCount } CR_MDSS_DP_WINDOW;
typedef struct { UINT64 Physical; UINTN Bytes; } CR_MDSS_DP_VIEW;
typedef struct {
  VOID *Context;
  UINT32 Soc;
  CR_MDSS_DP_VIEW View[CrMdssDpWindowCount];
  CR_STATUS (*Ready)(VOID *Context, CONST VOID *StableOwner);
  CR_STATUS (*Read32)(VOID *Context, CR_MDSS_DP_WINDOW, UINT32 Offset, UINT32 *Value);
  CR_STATUS (*Write32)(VOID *Context, CR_MDSS_DP_WINDOW, UINT32 Offset, UINT32 Value);
  CR_STATUS (*Fence)(VOID *Context);
  CR_STATUS (*DelayUs)(VOID *Context, UINT32 Microseconds);
  /* Actual shared-PHY DP owner clears its own PHY AUX interrupt latches.
     PHY version-specific registers are not part of the DP AUX view. */
  CR_STATUS (*ClearPhyInterrupts)(VOID *Context, CONST VOID *StableOwner);
  /* Optional complete IRQ transport group. All absent preserves native
     polling compatibility. ArmKick must publish the exact attempt and write
     GO under the real parent interrupt lock; no generic DIRQL callback.
     Wait is PASSIVE and consumes only that generation. Disarm follows actual
     GO-off/reset and drains/discards the exact attempt before any retry. */
  CR_STATUS (*ArmKick)(VOID *Context, CONST VOID *StableOwner, UINT32 Control,
                      UINT64 *Generation);
  CR_STATUS (*Wait)(VOID *Context, CONST VOID *StableOwner, UINT64 Generation,
                   UINT32 TimeoutUs, UINT32 *RawInterrupt);
  CR_STATUS (*Disarm)(VOID *Context, CONST VOID *StableOwner, UINT64 Generation);
} CR_MDSS_DP_AUX_IO;

typedef struct {
  CR_MDSS_DP_AUX_IO Io;
  CONST VOID *Owner;
  UINT32 LastInterrupt, Attempts;
  UINT64 IrqGeneration;
  CR_STATUS LastStatus;
  BOOLEAN Initialized, Enabled, Touched, CleanupPending, Stopped;
} CR_MDSS_DP_AUX;

/* Caller provides zeroed permanent storage, exclusive outer serialization and
   a stable owner with actual mapped AHB/AUX, clock/domain/rail/interconnect and
   shared-PHY DP leases. Ready requalifies those leases before every callback.
   Windows calls run at PASSIVE_LEVEL. This library neither acquires resources
   nor retires native/foreign writers. AUX reset also clears HPD interrupts:
   caller must serialize with the sole DP HPD/IRQ/controller owner.
   No link/scanout/SMMU/USB stop or DMA grant follows from any AUX success. */
CR_STATUS CrMdssDpAuxInit(CR_MDSS_DP_AUX *, CONST CR_MDSS_DP_AUX_IO *, CONST VOID *StableOwner);
CR_STATUS CrMdssDpAuxEnable(CR_MDSS_DP_AUX *, CONST VOID *StableOwner);
/* Native: 20-bit DPCD address, 1..16 bytes. I2C: 7-bit slave, 1..124 bytes.
   I2C writes retain the bus; reads send STOP, matching the native EDID path.
   Input is copied before MMIO; read output remains unchanged unless the entire
   transfer completes with verified FIFO indices and a final current witness.
   Callback/nonfinal/posted-write failure retains CleanupPending. Stop is then
   required before another Enable/Transfer. A fully reset timeout is retryable.
   At most seven transactions, each bounded to 25ms, plus reset/defer delays. */
CR_STATUS CrMdssDpAuxTransfer(CR_MDSS_DP_AUX *, CONST VOID *StableOwner,
  BOOLEAN Native, BOOLEAN Read, UINT32 Address, UINT8 *Buffer, UINTN Bytes);
/* Complete block checksum and all chunks qualify before output publication. */
CR_STATUS CrMdssDpAuxReadEdid(CR_MDSS_DP_AUX *, CONST VOID *StableOwner,
  UINT8 Block, UINT8 *Data, UINTN DataBytes);
/* Service DPCD IRQ vector; unsupported automated compliance request is NAKed.
   This is a PASSIVE AUX transaction, not an ISR/parent IRQ acknowledgement. */
CR_STATUS CrMdssDpAuxServiceIrq(CR_MDSS_DP_AUX *, CONST VOID *StableOwner);
/* Off-only retirement: GO clear, completed reset, AUX enable/reset off and
   owned packed AHB masks off by fresh readback. Preserves the foreign HPD mask.
   Failure retains all metadata and caller prerequisites; exact retries use
   the same owner/IO. Does not release resources or certify DP DMA/link stop. */
CR_STATUS CrMdssDpAuxStop(CR_MDSS_DP_AUX *, CONST VOID *StableOwner);
/* Metadata-only, after exact Stop (or never touched). Caller drains every use
   before this call and keeps callback storage alive through successful return. */
CR_STATUS CrMdssDpAuxUninit(CR_MDSS_DP_AUX *, CONST VOID *StableOwner);
