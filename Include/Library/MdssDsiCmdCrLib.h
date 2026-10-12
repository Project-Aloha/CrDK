/* Explicit SM8250 command-mode display, separate from the video ABI.
 * Source: Qualcomm SM8250 DSI2.4/PHY4.1, Linux DPU6.0 and selected HDK8245 FDT.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssDscCrLib.h>
#define CR_MDSS_CMD_REVISION 1U
#define CR_MDSS_CMD_SM8250 0x8250U
#define CR_MDSS_CMD_FRAME_DONE (1U << 0)
#define CR_MDSS_CMD_TE (1U << 1)
#define CR_MDSS_CMD_UNDERRUN (1U << 2)
#define CR_MDSS_CMD_EVENTS 7U
#define CR_MDSS_CMD_CAP_COMMAND (1U << 0)
#define CR_MDSS_CMD_CAP_DPHY (1U << 1)
#define CR_MDSS_CMD_CAP_LEGACY_DSC (1U << 2)
#define CR_MDSS_CMD_RESET_MAX 8U
#define CR_MDSS_CMD_ON_MAX 32U
#define CR_MDSS_CMD_OFF_MAX 8U
#define CR_MDSS_CMD_PAYLOAD_MAX 128U

typedef enum { CrMdssCmdDpu, CrMdssCmdHost, CrMdssCmdPhy, CrMdssCmdVbif, CrMdssCmdWindows } CR_MDSS_CMD_WINDOW;
typedef struct {
  UINT32 Revision, Soc, Controller, Lanes;
  UINT32 Width, Height, Stride, RefreshHz;
  UINT32 HSync, HBackPorch, HFrontPorch, VSync, VBackPorch, VFrontPorch;
  UINT32 JitterNumerator, JitterDenominator, FrameThresholdUs;
  UINT64 LaneBitHz, VsyncHz;
  CR_MDSS_DSC_PARAMS Dsc;
} CR_MDSS_CMD_CONFIG;
/* A narrow view of the shared SoC catalog, not a board selector. Unknown IP
 * revisions remain unsupported even when their register windows match. */
typedef struct {
  UINT32 Soc, DpuMajor, DpuMinor, DsiMajor, DsiMinor, PhyMajor, PhyMinor, Flags;
} CR_MDSS_CMD_CAPABILITIES;
typedef struct { BOOLEAN High; UINT32 DelayUs; } CR_MDSS_CMD_RESET;
typedef struct {
  UINT8 Type;
  BOOLEAN LowPower;
  UINT16 Bytes;
  UINT32 DelayUs;
  UINT8 Payload[CR_MDSS_CMD_PAYLOAD_MAX];
} CR_MDSS_CMD_PACKET;
/* Provider-selected immutable panel data. InitProfile captures it by value;
 * no transient command payload or reset-array pointer is borrowed. Native
 * resources and physical panel identity remain the provider's authority. */
typedef struct {
  UINT32 Revision;
  CR_MDSS_CMD_CAPABILITIES Hardware;
  CR_MDSS_CMD_CONFIG Config;
  UINT32 PhyTimings[14];
  UINT32 ResetCount, OnCount, OffCount, OffResetDelayUs;
  CR_MDSS_CMD_RESET Reset[CR_MDSS_CMD_RESET_MAX];
  CR_MDSS_CMD_PACKET On[CR_MDSS_CMD_ON_MAX], Off[CR_MDSS_CMD_OFF_MAX];
  UINT32 BrightnessMaximum;
  UINT8 BrightnessCommand, BrightnessBytes;
  BOOLEAN BrightnessMsbFirst;
} CR_MDSS_CMD_PROFILE;
typedef struct {
  CR_MDSS_CMD_CONFIG Config;
  CR_MDSS_CMD_CAPABILITIES Hardware;
  CR_MDSS_DSC_CONFIG Dsc;
  /* Requested nominal rates. PLL fractional quantization must be verified
   * against actual divider/fraction readback; these are not measurements. */
  UINT64 MinimumLaneBitHz, LaneBitHz, ByteHz, ByteInterfaceHz, PixelHz, VcoHz;
  UINT32 FrameUs, TransferUs, GuardUs, PllPostDivider, PhyPostDivider, PixelDivider;
  UINT32 HostClockTiming, PhyTimings[14], Htotal, Vtotal;
  UINT32 PacketBytes, PacketsPerLine, PayloadWidth, EndOfLineBytes;
} CR_MDSS_CMD_PLAN;
/* The profile is exact and immutable. It never identifies arbitrary R66451
 * nodes as this board, nor changes the active FDT/boot selection. */
VOID CrMdssCmdHdk8245Config(CR_MDSS_CMD_CONFIG *);
CR_STATUS CrMdssCmdBuildPlan(CONST CR_MDSS_CMD_CONFIG *, CR_MDSS_CMD_PLAN *);
/* Compatibility reference data only. The common native frontend must pass
 * its actual selected target/provider profile to InitProfile. */
VOID CrMdssCmdR66451Profile(CR_MDSS_CMD_PROFILE *);
CR_STATUS CrMdssCmdBuildPlanProfile(CONST CR_MDSS_CMD_PROFILE *, CR_MDSS_CMD_PLAN *);

typedef struct {
  VOID *Context, *Cpu;
  UINTN Bytes;
  UINT64 DeviceAddress;
  /* Real retained DMA mapping for this named engine and exact range. */
  CR_STATUS (*Ready)(VOID *, CR_MDSS_CMD_WINDOW Engine);
  CR_STATUS (*Publish)(VOID *, VOID *Cpu, UINTN Bytes);
} CR_MDSS_CMD_MEMORY;
typedef struct {
  VOID *Context;
  UINT64 Physical[CrMdssCmdWindows];
  UINTN Bytes[CrMdssCmdWindows];
  /* Caller holds exclusive MMIO, native retirement, power, clock, ICB and
   * SMMU ownership for this exact incarnation before every operation. */
  CR_STATUS (*Ready)(VOID *, CR_MDSS_CMD_WINDOW);
  CR_STATUS (*Read32)(VOID *, CR_MDSS_CMD_WINDOW, UINT32 Offset, UINT32 *);
  CR_STATUS (*Write32)(VOID *, CR_MDSS_CMD_WINDOW, UINT32 Offset, UINT32);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *, UINT32);
  /* On configures the real SM8250 ClockExternal provider from Plan. A
   * failure may retain Owned. Caller must retain provider storage/leases. */
  CR_STATUS (*ExternalClocks)(VOID *, CONST CR_MDSS_CMD_PLAN *, BOOLEAN On, BOOLEAN *Owned);
  /* Board owns reset75, TE66 and exact L14A/L11C/SWIRE resources. Logical
   * AB/IBB regulator disable alone is not a physical panel-off fence. */
  CR_STATUS (*PanelPower)(VOID *, BOOLEAN On, BOOLEAN *Owned);
  CR_STATUS (*PanelReset)(VOID *, BOOLEAN High);
  /* Native adapter uses OSKAL IRQ operations. Off seals and drains native
   * ISR/deferred callbacks before returning Owned=FALSE. It does not stop
   * hardware DMA. IRQ dispatch must serialize with the core via its owner.
   * Never wait while holding a lock needed by IRQ/deferred work: seal, return
   * NOT_READY with Owned=TRUE, and let the owner drain outside serialization
   * before retrying Stop. Elapsed time alone does not certify rundown. */
  CR_STATUS (*Events)(VOID *, BOOLEAN On, BOOLEAN *Owned);
  /* EBS only: permanently seal/mask and drain ISR/deferred writers without
   * allocation, unregister or event close. The real IRQ lease stays owned. */
  CR_STATUS (*FreezeEvents)(VOID *,BOOLEAN *Frozen);
} CR_MDSS_CMD_IO;
typedef struct {
  CR_MDSS_CMD_PLAN Plan;
  CR_MDSS_CMD_PROFILE Profile;
  CR_MDSS_CMD_IO Io;
  CR_MDSS_CMD_MEMORY Frame, Commands;
  BOOLEAN Initialized, Starting, Active, CleanupPending;
  BOOLEAN ExternalOwned, PanelOwned, IrqOwned;
  BOOLEAN PhyTouched, HostTouched, PanelTouched, DscTouched, DpuTouched;
  BOOLEAN CommandInFlight, FrameInFlight, AutoRefresh;
  BOOLEAN FirstFrameDone, Faulted;
  BOOLEAN DpuForceTouched, DpuHaltTouched;
  UINT32 DpuOriginalForce;
  BOOLEAN DpuDebugTouched;
  UINT32 DpuOriginalPeriphDebug, DpuOriginalDsppDebug;
  UINT64 FrameSerial, TeSerial;
  BOOLEAN Frozen,FreezeComplete,IrqFrozen;
  UINT32 FreezeStage;
} CR_MDSS_CMD;
/* All calls serialized by native owner; no library lock, timer or worker
 * impersonates real OSKAL IRQ/DMA/resource ownership. ISR is a short bounded
 * source reader/acknowledger; any recovery is deferred by the native owner. */
CR_STATUS CrMdssCmdInit(CR_MDSS_CMD *, CONST CR_MDSS_CMD_IO *, CONST CR_MDSS_CMD_CONFIG *,
                       CONST CR_MDSS_CMD_MEMORY *Frame, CONST CR_MDSS_CMD_MEMORY *Commands);
CR_STATUS CrMdssCmdInitProfile(CR_MDSS_CMD *, CONST CR_MDSS_CMD_IO *,
                       CONST CR_MDSS_CMD_PROFILE *, CONST CR_MDSS_CMD_MEMORY *Frame,
                       CONST CR_MDSS_CMD_MEMORY *Commands);
/* First Start may return NOT_READY after an accepted kickoff. The native
 * owner releases serialization for IRQ delivery; later Start only observes
 * FirstFrameDone and completes publication, never repeats an armed kickoff. */
CR_STATUS CrMdssCmdStart(CR_MDSS_CMD *);
CR_STATUS CrMdssCmdHandleIrq(CR_MDSS_CMD *, UINT32 *Events);
CR_STATUS CrMdssCmdPublish(CR_MDSS_CMD *, UINTN Offset, UINTN Bytes);
CR_STATUS CrMdssCmdSetBrightness(CR_MDSS_CMD *, UINT32 Level);
/* Stop retains all touched state/dependencies on any physical fence failure.
 * EBS handoff is intentionally not encoded as the old video handoff ABI. */
CR_STATUS CrMdssCmdStop(CR_MDSS_CMD *);
CR_STATUS CrMdssCmdUninit(CR_MDSS_CMD *);
/* EBS pre-revocation fence, distinct from panel OFF or an OS display handoff.
 * Keeps IRQ registration, power, PLL/PHY, clocks, buffers and native owners.
 * Stops kickoff/TE/autorefresh, obtains a fresh physical XIN halt and resets
 * the command DMA host. No supplier/off RPC, allocation or lifetime change.
 * Failure retains an independent retry journal; only Freeze may continue. */
CR_STATUS CrMdssCmdFreeze(CR_MDSS_CMD *);
