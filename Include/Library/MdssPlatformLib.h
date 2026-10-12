/* HDK8450 qualified resource/DSI PHY/LT9611 platform owner.
 * SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/MdssCrLib.h>

#define CR_MDSS_PLATFORM_REVISION 1U
#define CR_MDSS_PLATFORM_HDK8450 0x84500001U
#define CR_MDSS_PHY_BYTES 0x800U
#define CR_MDSS_DISPCC_BYTES 0x10000U
#define CR_MDSS_PLATFORM_CLOCKS 10U /* nine leaves plus MDSS GDSC */
#define CR_MDSS_PLATFORM_RAILS 3U
#define CR_MDSS_PLATFORM_PATHS 2U
/* These are independent verified facts, not a default "ready" flag. Native
 * means the boot producer supplied a final callback-retired ownership-transfer
 * record describing the inherited live/stopped path and retained leases;
 * DMA means the OS reservation/SMMU provider qualified all four SIDs,
 * read-only access, uncached CPU alias policy and durable POST lifetime.
 * Serial includes exclusive banked bridge access and GPIO pin ownership.
 * ExternalClock includes exclusive ClockCr-managed source/divider ownership.
 * Every provider incarnation must be current when this witness is returned. */
#define CR_MDSS_QUAL_NATIVE 1U
#define CR_MDSS_QUAL_DMA 2U
#define CR_MDSS_QUAL_SERIAL 4U
#define CR_MDSS_QUAL_EXTERNAL_CLOCK 8U
#define CR_MDSS_QUAL_PERMANENT 16U
#define CR_MDSS_QUAL_ALL 31U
typedef enum { CrMdssPlatformPhy, CrMdssPlatformDispCc } CR_MDSS_PLATFORM_WINDOW;
typedef struct {
  UINT32 Revision, Board;
  CR_MDSS_MODE Mode;
  UINT64 DpuPhysical, DsiPhysical, FrameBufferPhysical;
  UINTN FrameBufferBytes;
  UINT64 BootGeneration;
} CR_MDSS_PLATFORM_CONTRACT;
typedef struct {
  UINT32 Revision, Facts;
  UINT64 BootGeneration, NativeTransferId, DmaIncarnation;
  UINT64 SerialIncarnation, ExternalClockIncarnation, PermanentStoreId;
} CR_MDSS_PLATFORM_WITNESS;
typedef struct {
  UINT32 Controller;
  CONST CHAR8 *Name;
  UINT64 RateHz;
  BOOLEAN Gdsc;
} CR_MDSS_PLATFORM_CLOCK;
typedef struct { CONST CHAR8 *Name; UINT32 Millivolts; UINT8 Mode; } CR_MDSS_PLATFORM_RAIL;
typedef struct { CONST CHAR8 *Provider; UINT32 Source, Destination; } CR_MDSS_PLATFORM_PATH;
extern CONST CR_MDSS_PLATFORM_CLOCK CrMdssPlatformClocks[CR_MDSS_PLATFORM_CLOCKS];
extern CONST CR_MDSS_PLATFORM_RAIL CrMdssPlatformRails[CR_MDSS_PLATFORM_RAILS];
extern CONST CR_MDSS_PLATFORM_PATH CrMdssPlatformPaths[CR_MDSS_PLATFORM_PATHS];
extern CONST UINT16 CrMdssPlatformSids[4];

typedef struct {
  VOID *Context;
  /* Pure current-incarnation qualification, no allocation or hardware writes. */
  CR_STATUS (*Qualify)(VOID *, CONST CR_MDSS_PLATFORM_CONTRACT *, CR_MDSS_PLATFORM_WITNESS *);
  /* Exclusive callback-retired boot pipe AND bridge route claim. It may still
     scan out. On partial failure Owned is true and all inherited resource/DMA
     leases remain pinned until NativeFence completes under real access. */
  CR_STATUS (*NativeClaim)(VOID *, VOID *PermanentOwner, CONST CR_MDSS_PLATFORM_WITNESS *, BOOLEAN *Owned);
  /* Runs after new rail/ICB/access/domain/core votes and GDSC readback; stops
     every inherited pipe, fences DMA, retires PHY/PLL/bridge and old clock/DMA
     ownership. Failure retains exact progress, native mapping and dependencies
     for retry; no cold programming or vote withdrawal precedes success. */
  CR_STATUS (*NativeFence)(VOID *, VOID *PermanentOwner, CONST CR_MDSS_PLATFORM_WITNESS *);
  CR_STATUS (*NativeRelease)(VOID *, VOID *PermanentOwner);
  /* Get/validate each pre-reserved owner vote without MMIO or new capacity. */
  CR_STATUS (*ResourceReady)(VOID *, VOID *PermanentOwner);
  CR_STATUS (*Clock)(VOID *, VOID *PermanentOwner, UINT32 Index, BOOLEAN On);
  CR_STATUS (*Rail)(VOID *, VOID *PermanentOwner, UINT32 Index, BOOLEAN On);
  CR_STATUS (*Bandwidth)(VOID *, VOID *PermanentOwner, UINT32 Index, UINT64 Average, UINT64 Peak);
  /* ClockCr-owned byte0/pclk0 external source configure and byte interface /2.
     On=true selects sources 2/1 with HID=2, M=N=D=0, then enables the three
     leaves. On=false unwinds retained state in reverse order. The provider
     owns its register mappings/lock and partial-failure progress. */
  CR_STATUS (*ExternalClocks)(VOID *, VOID *PermanentOwner, UINT64 PixelHz, UINT64 ByteHz, BOOLEAN On);
  UINT32 (*Read32)(VOID *, CR_MDSS_PLATFORM_WINDOW, UINT32 Offset);
  VOID (*Write32)(VOID *, CR_MDSS_PLATFORM_WINDOW, UINT32 Offset, UINT32 Value);
  VOID (*Fence)(VOID *);
  VOID (*DelayUs)(VOID *, UINT32);
  /* Provider must map the complete reserved surface read-only for these exact
     SIDs. DeviceAddress is an OS-qualified IOVA, never derived from Physical.
     On partial error MappingOwned is true and Unmap remains callable. */
  CR_STATUS (*MapFrameBuffer)(VOID *, VOID *PermanentOwner, UINT64 Physical,
      UINTN Bytes, CONST UINT16 *Sids, UINT32 SidCount, UINT64 *DeviceAddress, BOOLEAN *MappingOwned);
  CR_STATUS (*UnmapFrameBuffer)(VOID *, VOID *PermanentOwner);
  /* Exclusive GPIO leases: 9=1.2V,109=3.3V,107=reset; no unqualified TLMM. */
  CR_STATUS (*Gpio)(VOID *, VOID *PermanentOwner, UINT32 Pin, BOOLEAN High);
  /* Exact QUP one-based instance10, addr0x2b, 400kHz. Provider qualifies
     exclusive live transport and a bounded repeated-start sequence; a native
     requested stretch field does not prove a hardware stretch timer. Read is
     write-offset + repeated-start one-byte read, write is one two-byte frame.
     No IRQ/other client may enter the banked bridge transaction concurrently. */
  CR_STATUS (*I2cWrite)(VOID *, VOID *PermanentOwner, UINT8 Register, UINT8 Value);
  CR_STATUS (*I2cRead)(VOID *, VOID *PermanentOwner, UINT8 Register, UINT8 *Value);
  /* Atomic durable publication: commit pins this provider's code, permanent
     state, native/clock/rail/ICB/serial/DMA leases through adapter removal.
     Failure changes no ownership. Reclaim fences the running POST pipe before
     dropping publication; failure leaves POST entirely owned and running. */
  CR_STATUS (*CommitPost)(VOID *, VOID *PermanentOwner, CONST CR_MDSS_PLATFORM_CONTRACT *, UINT64 DeviceAddress);
  CR_STATUS (*ReclaimPost)(VOID *, VOID *PermanentOwner);
  /* Exact assigned/mapped spans, immutable for this provider incarnation.
     Init preflights the complete sequence before the first hardware access. */
  UINTN PhyBytes, DispCcBytes;
} CR_MDSS_PLATFORM_IO;
typedef struct {
  CR_MDSS_PLATFORM_IO Io;
  CR_MDSS_PLATFORM_CONTRACT Config;
  CR_MDSS_PLATFORM_WITNESS Witness;
  VOID *Owner;
  UINT64 PixelRate, DeviceAddress;
  UINT32 HostClockTiming;
  BOOLEAN Initialized, NativeOwned, NativeFenced, MappingOwned, ExternalTouched;
  BOOLEAN ClockTouched[CR_MDSS_PLATFORM_CLOCKS], RailTouched[CR_MDSS_PLATFORM_RAILS];
  BOOLEAN PathTouched[CR_MDSS_PLATFORM_PATHS];
  BOOLEAN PhyTouched, BridgePowerTouched[2], ResetTouched, McuLockPending;
  BOOLEAN Ready, PanelReady, CleanupPending, PostOwned, ResourcesOnly;
} CR_MDSS_PLATFORM;

/* External PASSIVE serialization and stable nonpaged storage/code required.
 * Release is permitted only after consumer DMA stop/fence. Failed operations
 * retain their complete owner and dependencies until a successful retry.
 * Init/Query are pure; Query cannot establish absent OS/native qualification. */
CR_STATUS CrMdssPlatformInit(CR_MDSS_PLATFORM *, CONST CR_MDSS_PLATFORM_IO *, CONST CR_MDSS_PLATFORM_CONTRACT *);
CR_STATUS CrMdssPlatformQuery(CR_MDSS_PLATFORM *, CR_MDSS_PLATFORM_CONTRACT *);
CR_STATUS CrMdssPlatformAcquire(CR_MDSS_PLATFORM *, VOID *Owner, UINT64 *DeviceAddress,
                               UINT32 *HostClockTiming, BOOLEAN *Owned);
/* Same native/D0/access/rail/interconnect and read-only framebuffer lease,
   without legacy PHY or external-source programming. The extended DSI owner
   subsequently owns and stops those phases before Release. Acquisition kinds
   cannot be interchanged on a retained live/POST lease. */
CR_STATUS CrMdssPlatformAcquireResources(CR_MDSS_PLATFORM *,VOID *Owner,
    UINT64 *DeviceAddress,BOOLEAN *Owned);
/* Fences the broker's inherited POST scanout and assigns the exact retained
   resource lease to Owner, preserving all maps/PHY/clock/panel journals for
   the caller's complete pipeline stop. No cold programming or release here. */
CR_STATUS CrMdssPlatformReclaimPost(CR_MDSS_PLATFORM *,VOID *Owner,BOOLEAN *Owned);
CR_STATUS CrMdssPlatformPreparePanel(CR_MDSS_PLATFORM *, VOID *Owner);
CR_STATUS CrMdssPlatformUnpreparePanel(CR_MDSS_PLATFORM *, VOID *Owner);
CR_STATUS CrMdssPlatformRelease(CR_MDSS_PLATFORM *, VOID *Owner);
CR_STATUS CrMdssPlatformTransferToPost(CR_MDSS_PLATFORM *, VOID *Owner);
CR_STATUS CrMdssPlatformUninit(CR_MDSS_PLATFORM *);

/* Bounded SM8450 DSI0 shutdown under a real caller-owned native claim or
 * current Windows PHY lease. DMA and the DSI host must already be stopped.
 * This does not acquire ownership or withdraw resources. Ready qualifies the
 * same mapping/access incarnation before every operation; callbacks must not
 * reacquire the caller's operation lock. Failure retains all dependencies.
 * Off-only steps may be retried before cold programming; the caller must never
 * replay a completed inherited transfer against the new Windows PHY owner. */
typedef struct {
  VOID *Context;
  UINT64 PhyPhysical;
  UINTN PhyBytes;
  CR_STATUS (*Ready)(VOID *);
  CR_STATUS (*Read32)(VOID *, UINT32 Offset, UINT32 *Value);
  CR_STATUS (*Write32)(VOID *, UINT32 Offset, UINT32 Value);
  CR_STATUS (*Fence)(VOID *);
  CR_STATUS (*DelayUs)(VOID *, UINT32);
} CR_MDSS_DSI_PHY_FENCE_IO;
CR_STATUS CrMdssPlatformFenceDsiPhy(CONST CR_MDSS_DSI_PHY_FENCE_IO *Io);
