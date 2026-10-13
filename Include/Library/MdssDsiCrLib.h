/* Extended SM8450 DPHY/CPHY and bonded dual-controller DSI.
 * Android SM8450 display-driver and mainline msm DSI source adaptation.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssCrLib.h>
#include <Library/MdssDscCrLib.h>
#include <Library/ClockExternalLib.h>
#define CR_MDSS_DSI_REVISION 1U
#define CR_MDSS_DSI_LINKS 2U
typedef enum { CrMdssDphy=0, CrMdssCphy=1 } CR_MDSS_PHY_TYPE;
typedef enum { CrMdssExtDpu,CrMdssExtDsi0,CrMdssExtDsi1,
               CrMdssExtPhy0,CrMdssExtPhy1,CrMdssExtWindows } CR_MDSS_EXT_WINDOW;
typedef struct {
  UINT32 Revision, Controllers, FirstController;
  CR_MDSS_PHY_TYPE PhyType;
  /* Full panel raster. Lanes means physical DPHY data lanes or CPHY trios.
     Dual links split every horizontal field evenly, preserve vertical timing.
     This is bonded dual DSI, not a single controller's split-link mode. */
  CR_MDSS_MODE Raster;
  /* Authoritative full-raster pixel clock, consistent with RefreshQ16 within
     one rounding unit. DSI video requires positive sync polarity. */
  UINT64 PixelHz;
  UINT8 LaneMap[4]; /* one-hot physical data-lane map; permutation of 1,2,4,8 */
  BOOLEAN ContinuousClock, Dsc;
  UINT32 SlicesPerPacket;
  CR_MDSS_DSC_CONFIG Compression;
} CR_MDSS_DSI_MODE;
typedef struct {
  CR_MDSS_DSI_MODE Mode;
  CR_MDSS_MODE Link[2];
  UINT64 PixelHz, ByteHz, SymbolHz, VcoHz, PixelSourceHz;
  UINT32 PllPostDivider, PhyPostDivider, PixelDivider, PixelMux, PixelM, PixelN;
  UINT32 ByteInterfaceDivider;
  UINT32 Timings[14], HostClockTiming;
  UINT32 PayloadWidth, PacketBytes, PacketsPerLine, EndOfLineBytes;
} CR_MDSS_DSI_PLAN;
/* Pure immutable topology/payload/clock/timing preflight. RGB888 host transport
   only; DSC RGB10 rate-control is available separately from 10-bit scanout. */
CR_STATUS CrMdssDsiBuildPlan(CONST CR_MDSS_DSI_MODE *, CR_MDSS_DSI_PLAN *);
/* Rebuilds immutable mode preflight and converts the exact PHY clock output
   into the provider's neutral plan; no frequency is inferred from metadata. */
CR_STATUS CrMdssDsiClockPlan(CONST CR_MDSS_DSI_PLAN *,UINT32 Controller,CR_CLOCK_DSI_PLAN *);
typedef struct {
  CR_CLOCK_EXTERNAL *Provider;
  VOID *Owner, *Context;
  /* Current native resource/claim admission, not a cached capability bit. */
  CR_STATUS (*Ready)(VOID *);
} CR_MDSS_DSI_CLOCK_OWNER;
/* Native adapter for an already reserved real ClockLib provider. Caller owns
   provider serialization and retains all resources after partial failure.
   Windows uses the same plan through its referenced SetPlan interface. */
CR_STATUS CrMdssDsiClockOwnerSet(VOID *,UINT32 Controller,
    CONST CR_MDSS_DSI_PLAN *,BOOLEAN On,BOOLEAN *Owned);
/* Caller holds exact exclusive mappings, power/clock/domain/rail leases and
   retired-native owner before Prepare. Ready validates these same resources
   before every IO. External byte/pixel clocks must be configured from Plan
   and remain gated until PHY preparation succeeds; pixel M/N must be honored.
   No physical address is used as a DMA address by this API. */
typedef struct {
  VOID *Context;
  UINT64 Physical[CrMdssExtWindows];
  UINTN Bytes[CrMdssExtWindows];
  CR_STATUS (*Ready)(VOID *, CR_MDSS_EXT_WINDOW);
  CR_STATUS (*Read32)(VOID *, CR_MDSS_EXT_WINDOW, UINT32 Offset, UINT32 *Value);
  CR_STATUS (*Write32)(VOID *, CR_MDSS_EXT_WINDOW, UINT32 Offset, UINT32 Value);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *, UINT32);
  /* Actual exclusive external clock provider, not raw DISPCC writes. Owned
     remains true after partially accepted configuration until off succeeds. */
  CR_STATUS (*ExternalClocks)(VOID *, UINT32 Controller,
      CONST CR_MDSS_DSI_PLAN *, BOOLEAN On, BOOLEAN *Owned);
} CR_MDSS_EXT_IO;
/* Retained command buffer from an actual DMA provider. Ready proves the
   immutable CPU alias and device read mapping for the named DSI controller.
   Caller retains it through every failed transfer until Stop succeeds; this
   library never derives DeviceAddress from a CPU pointer/physical address. */
typedef struct {
 VOID *Context, *Cpu;
 UINTN Bytes;
 UINT64 DeviceAddress;
 CR_STATUS (*Ready)(VOID *,UINT32 Controller);
 CR_STATUS (*Publish)(VOID *,VOID *Cpu,UINTN Bytes);
} CR_MDSS_DSI_COMMAND_MEMORY;
typedef struct {
  CR_MDSS_EXT_IO Io;
  CR_MDSS_DSI_PLAN Plan;
  BOOLEAN Initialized, Prepared, Active, CleanupPending;
  BOOLEAN PhyTouched[2], HostTouched[2], ExternalTouched[2];
  BOOLEAN CommandTouched[2], PpsSent[2], CompressionEnabled[2];
  CR_MDSS_DSI_COMMAND_MEMORY CommandMemory;
  BOOLEAN CommandsConfigured;
} CR_MDSS_DSI;
CR_STATUS CrMdssDsiInit(CR_MDSS_DSI *, CONST CR_MDSS_EXT_IO *, CONST CR_MDSS_DSI_MODE *);
CR_STATUS CrMdssDsiPrepare(CR_MDSS_DSI *);
/* Pure immutable buffer configuration before Prepare. Long packets that
   exceed the hardware's 64-byte FIFO require this qualified mapping. */
CR_STATUS CrMdssDsiSetCommandMemory(CR_MDSS_DSI *,CONST CR_MDSS_DSI_COMMAND_MEMORY *);
/* Explicit packet write while command host is prepared and video is off.
   Supported packet types: generic/DCS short/long, DSC PPS/compression-mode.
   A timeout/partial write forbids another packet/Enable until Stop succeeds. */
CR_STATUS CrMdssDsiWritePacket(CR_MDSS_DSI *,UINT32 Controller,UINT8 Datatype,
    CONST UINT8 *Payload,UINTN Bytes,BOOLEAN LowPower);
CR_STATUS CrMdssDsiSendPps(CR_MDSS_DSI *,UINT8 Identifier);
/* Panel preparation/PPS transmission and DPU/DSC programming precede Enable.
   Caller must enable the synchronized DPU timing engine only afterward. */
CR_STATUS CrMdssDsiEnable(CR_MDSS_DSI *);
/* Stop video, retain prepared command transport for panel off commands. */
CR_STATUS CrMdssDsiDisableVideo(CR_MDSS_DSI *);
/* Caller first stops/fences DPU DMA. Failed stop retains exact touched windows
   and all caller dependencies; retry off-only before new Windows programming. */
CR_STATUS CrMdssDsiStop(CR_MDSS_DSI *);
CR_STATUS CrMdssDsiUninit(CR_MDSS_DSI *);
/* Internal cross-file PHY implementation; no standalone readiness grant. */
CR_STATUS CrMdssDsiPhyPrepare(CR_MDSS_DSI *);
CR_STATUS CrMdssDsiPhyStop(CR_MDSS_DSI *);
