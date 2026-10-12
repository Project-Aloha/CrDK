/* Exact SM8250 boot-display admission and physical retirement.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <oskal/cr_types.h>
#include <oskal/cr_status.h>

#define CR_MDSS_BOOT8250_REVISION 1U
#define CR_MDSS_BOOT8250_MAX_STREAMS 4U
#define CR_MDSS_BOOT8250_IRQ_BANKS 12U

typedef enum {
  CrMdssBootTop, CrMdssBootDpu, CrMdssBootVbif, CrMdssBootHost0,
  CrMdssBootPhy0, CrMdssBootHost1, CrMdssBootPhy1, CrMdssBootRegDma,
  CrMdssBootWindows
} CR_MDSS_BOOT_WINDOW;

typedef struct {
  UINT32 Top, Intf, Dsc, Wb, Cwb, Cdm, Merge, Flush, Reset, RegDmaTrigger;
  UINT32 IntfMaster, RotTop, RotStart;
  UINT32 Layer[6][4];
} CR_MDSS_BOOT_CTL;
typedef struct {
  UINT32 Decimation, SourceSize, SourceXy, OutputSize, OutputXy;
  UINT32 Address[4], Stride[2], Format, Unpack, OpMode, Secure, MultiRect;
  UINT32 PixelExtension[9];
} CR_MDSS_BOOT_SSPP;
typedef struct {
  UINT32 Enable, Config, TeEnable, TeConfig, AutoRefresh, LineCount, Mux;
  UINT32 Config2, SplitLink, CommandTrigger;
} CR_MDSS_BOOT_INTF;
typedef struct {
  UINT32 Smr, S2cr, Sctlr, Cbar, Cba2r, Tcr, Tcr2, Actlr;
  UINT64 Ttbr0, Ttbr1;
  UINT16 Index, Bank;
  UINT32 Reserved;
} CR_MDSS_BOOT_STREAM;
typedef struct {
  UINT64 ProducerGeneration, DeviceAddress, PhysicalAddress, Bytes;
  UINT32 Count, Reserved;
  CR_MDSS_BOOT_STREAM Streams[CR_MDSS_BOOT8250_MAX_STREAMS];
} CR_MDSS_BOOT_DMA_OBSERVATION;

typedef struct {
  UINT32 Revision, DpuVersion, MdssPending;
  CR_MDSS_BOOT_CTL Ctl[6];
  CR_MDSS_BOOT_SSPP Sspp[8]; /* FDT order VIG0..3, DMA0..3. */
  CR_MDSS_BOOT_INTF Intf[4];
  UINT32 IntfStatus[4];
  UINT32 PpDsc[6], PpConfig[6], PpAutoRefresh[6], DscMux[4];
  UINT32 DscWords[4][4], LmOutput[6];
  UINT32 InterfaceSelect, SplitEnable, SplitUpper, SplitLower;
  UINT32 PpbControl[4], PpbConfig[2], DceSelect;
  UINT32 HostControl[2], HostStatus[2], HostInterrupt[2];
  UINT32 HostSplitLink[2];
  UINT32 PhyPll[2], PhyControl[2], PhyClock[2], PhyBuffer[2];
  UINT32 PhyRbuf[2];
  UINT32 IrqEnable[CR_MDSS_BOOT8250_IRQ_BANKS];
  UINT32 IrqStatus[CR_MDSS_BOOT8250_IRQ_BANKS];
  UINT32 RegDmaMode, RegDmaQueues[6][4], RegDmaStatus[5];
  UINT32 VbifRequest, VbifAck, Force[4];
  /* Derived only after complete route admission; never supplied by caller. */
  UINT32 SourceIndex, SourceOffset, Xin, ForceOffset, ForceMask, FlushMask;
  UINT32 Width, Height, Stride, FrameUnpack;
  UINT32 PixelRedMask, PixelGreenMask, PixelBlueMask, PixelReservedMask;
  UINT64 FrameDevice, FrameBytes;
  CR_MDSS_BOOT_DMA_OBSERVATION Dma;
} CR_MDSS_BOOT_OBSERVATION;

typedef CR_STATUS (*CR_MDSS_BOOT_READ32)(VOID *Context,
  CR_MDSS_BOOT_WINDOW Window,UINT32 Offset,UINT32 *Value);

/* Pure inventory for a native owner preserving the current boot route.
 * The callback must independently qualify retained physical MMIO access on
 * every read. Two complete inventories reject changing static route state;
 * IRQ/status/line-count progression is allowed. Failure clears Out. No
 * fake lease, clock/panel programming or DMA observation is performed. */
CR_STATUS CrMdssBoot8250ReadInventory(VOID *Context,CR_MDSS_BOOT_READ32 Read32,
  CR_MDSS_BOOT_OBSERVATION *Out);
/* Admit the strict existing single DSI0 command raster on the sole base
 * plane with source alpha disabled and enabled INTF1 autorefresh, then expose
 * its source geometry/device range and byte packing.
 * Android selectors 0/1/2/3 mean G/B/R/A. Success is only a hardware-route
 * proof; Dma remains untouched and a SMMU producer must independently prove
 * every byte maps into the retained splash reservation before GOP writes.
 * Derived fields are cleared before every probe, including failed retries. */
CR_STATUS CrMdssBoot8250ProbePreservedRoute(CR_MDSS_BOOT_OBSERVATION *);

typedef struct {
  VOID *Context;
  UINT64 Physical[CrMdssBootWindows];
  UINTN Bytes[CrMdssBootWindows];
  /* Acquire a real inherited-access lease, retaining boot rails, ICB and
   * unchanged clocks. It never retunes a live source. Failure reports actual
   * ownership; output generation identifies this same native incarnation. */
  CR_STATUS (*Access)(VOID *,BOOLEAN On,BOOLEAN *Owned,UINT64 *Generation);
  /* Validate the retained lease and exclusive software writer ownership on
   * every call. Before retirement all inherited clocks/rails remain applied.
   * After branches/PHY are off, bus access and their owning leases remain. */
  CR_STATUS (*Ready)(VOID *,UINT64 Generation,CR_MDSS_BOOT_WINDOW);
  CR_STATUS (*Read32)(VOID *,CR_MDSS_BOOT_WINDOW,UINT32 Offset,UINT32 *Value);
  CR_STATUS (*Write32)(VOID *,CR_MDSS_BOOT_WINDOW,UINT32 Offset,UINT32 Value);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *,UINT32 Us);
  /* SMMU producer-owned pure observation under its lock. Observe all matching
   * SMRs, reject overlapping outsiders/shared CBs, and prove the full device
   * range resolves into the exact retained boot splash reservation. Callback
   * admits only source-qualified stage-1 or identity bypass contexts; stage-2,
   * nested and static-secure contexts are unsupported. A page-table walk must
   * prove every byte, including crossings, without fabricated identity data.
   * must not transfer/reset/attach streams or synthesize identity mapping.
   * The eventual SMMU commit independently revalidates this inventory. */
  CR_STATUS (*ObserveDma)(VOID *,UINT64 Device,UINT64 Bytes,
                          CR_MDSS_BOOT_DMA_OBSERVATION *);
  /* Seal all former native IRQ/work writers and drain outside their locks.
   * A pending drain returns NOT_READY; caller retries without losing journal.
   * Never wait for a worker that needs the caller's serialization lock.
   * This is software retirement only; this core masks/fences actual hardware. */
  CR_STATUS (*DrainWriters)(VOID *);
  /* Exclusive GPIO75 reset lease and exact low readback, preserving rails. */
  CR_STATUS (*PanelResetOff)(VOID *);
  CR_STATUS (*PanelResetReadyOff)(VOID *); /* Pure owned low-level readback. */
  /* Clock owner gates/readbacks inherited DSI0 byte/pixel branches only after
   * our DMA/host fence. Repeated calls confirm off; other dependencies stay. */
  CR_STATUS (*ExternalClocksOff)(VOID *);
  CR_STATUS (*ExternalClocksReadyOff)(VOID *); /* Pure owned gate readback. */
} CR_MDSS_BOOT_IO;

typedef struct {
  CR_MDSS_BOOT_IO Io;
  CR_MDSS_BOOT_OBSERVATION Before;
  UINT64 Generation;
  UINT32 Stage;
  CR_STATUS LastStatus;
  UINT32 ForceOriginal, RegDmaForceOriginal;
  UINT32 DebugPeriphOriginal, DebugDsppOriginal;
  BOOLEAN Initialized, AccessOwned, Admitted, Touched, Retired;
  BOOLEAN ForceTouched, RegDmaForceTouched, WritersDrained;
  BOOLEAN DebugTouched;
} CR_MDSS_BOOT8250;

/* Caller owns passive serialization. Init is pure, and requires zeroed state.
 * No entry point mutates SMMU or claims a cold command pipeline is running. */
CR_STATUS CrMdssBoot8250Init(CR_MDSS_BOOT8250 *,CONST CR_MDSS_BOOT_IO *);
/* Retained access, pure hardware/DMA observation, then strict source-derived
 * admission. Unsupported live state remains untouched and can be diagnosed. */
CR_STATUS CrMdssBoot8250Observe(CR_MDSS_BOOT8250 *);
/* Physical retirement with retained stage journals. Failure retains access,
 * boot memory and stream inventory. Only physical-fence checks allow success;
 * SMMU producer transfer and cold-source ownership are subsequent operations. */
CR_STATUS CrMdssBoot8250Retire(CR_MDSS_BOOT8250 *);
CR_STATUS CrMdssBoot8250CheckFence(CR_MDSS_BOOT8250 *);
/* Abort is permitted only before any hardware retirement write/callback.
 * Once touched, retained owner/context survives through cold takeover/EBS. */
CR_STATUS CrMdssBoot8250Abort(CR_MDSS_BOOT8250 *);
