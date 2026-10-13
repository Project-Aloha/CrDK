/* Immutable native display ownership record. SPDX-License-Identifier: MIT */
#pragma once
#include <Library/smmu.h>
#include <stddef.h>

#define CR_MDSS_NATIVE_SIGNATURE 0x4853444DU /* MDSH */
#define CR_MDSS_NATIVE_REVISION 1U
#define CR_MDSS_NATIVE_LOCATOR_SIGNATURE 0x4653444DU /* MDSF */
#define CR_MDSS_NATIVE_LOCATOR_REVISION 1U
#pragma pack(push,1)
typedef struct {
  UINT32 Signature, Length;
  UINT8 Revision, Checksum, OemId[6], OemTableId[8];
  UINT32 OemRevision, CreatorId, CreatorRevision, WireRevision;
  UINT64 RecordPhysical;
  UINT32 RecordLength, Reserved;
  UINT64 RecordGeneration;
} CR_MDSS_NATIVE_LOCATOR;
#pragma pack(pop)
CR_STATIC_ASSERT(sizeof(CR_MDSS_NATIVE_LOCATOR)==64,"MDSS ACPI locator size");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_LOCATOR,WireRevision)==36,"MDSS locator revision offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_LOCATOR,RecordPhysical)==40,"MDSS locator address offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_LOCATOR,RecordLength)==48,"MDSS locator length offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_LOCATOR,RecordGeneration)==56,"MDSS locator generation offset");
#define CR_MDSS_NATIVE_PLATFORM_SM8450_HDK 1U
#define CR_MDSS_NATIVE_DSI 1U
#define CR_MDSS_NATIVE_DP 2U
#define CR_MDSS_NATIVE_BRIDGE 4U
#define CR_MDSS_NATIVE_SMMU 8U
#define CR_MDSS_NATIVE_PHY 16U
#define CR_MDSS_NATIVE_ALL_OWNERS 31U
#define CR_MDSS_NATIVE_ROUTES 2U
#define CR_MDSS_NATIVE_CLOCKS 32U
#define CR_MDSS_NATIVE_RAILS 8U
#define CR_MDSS_NATIVE_PATHS 4U
#define CR_MDSS_NATIVE_DOMAINS 4U
#define CR_MDSS_NATIVE_STREAMS 4U

#define CR_MDSS_NATIVE_PENDING 0U
#define CR_MDSS_NATIVE_FINAL 1U
#define CR_MDSS_NATIVE_ABSENT 0U
#define CR_MDSS_NATIVE_LIVE 1U
#define CR_MDSS_NATIVE_STOPPED 2U
#define CR_MDSS_NATIVE_UNCERTAIN 3U
#define CR_MDSS_NATIVE_OWNED 1U
#define CR_MDSS_NATIVE_RETAINED_ERROR 2U
#define CR_MDSS_NATIVE_FORMAT_BGRX8888 1U
/* PHY readiness and dispatched-register journals, never ownership grants. */
#define CR_MDSS_NATIVE_PHY_COMMON 1U
#define CR_MDSS_NATIVE_PHY_USB 2U
#define CR_MDSS_NATIVE_PHY_DP 4U
#define CR_MDSS_NATIVE_PHY_FSA 8U
#define CR_MDSS_NATIVE_PHY_USB_SS 16U
#define CR_MDSS_NATIVE_PHY_READY_MASK 31U
#define CR_MDSS_NATIVE_PHY_TOUCHED_MASK 15U
#define CR_MDSS_NATIVE_MAPPING_UNKNOWN 0U
#define CR_MDSS_NATIVE_MAPPING_PRESERVED 1U
#define CR_MDSS_NATIVE_MAPPING_REVOKED 2U
#define CR_MDSS_NATIVE_MAPPING_NONE 3U

/* Demands are the exact native client's retained journal. They are not OS
   handles or a permission to drop an overlapping firmware/OS owner's vote.
   Names are zero-terminated ASCII with the entire unused suffix zeroed. */
typedef struct {
  UINT32 Owner, Flags;
  CHAR8 Controller[8], Name[48];
  UINT64 RateHz;
} CR_MDSS_NATIVE_CLOCK;
typedef struct {
  UINT32 Owner, Flags;
  CHAR8 Name[24];
  UINT32 Millivolts, Mode;
} CR_MDSS_NATIVE_RAIL;
typedef struct {
  UINT32 Owner, Flags, Source, Destination;
  CHAR8 Provider[24];
  UINT64 Average, Peak;
} CR_MDSS_NATIVE_PATH;
typedef struct {
  UINT32 Owner, Flags;
  CHAR8 Controller[8], Name[32];
} CR_MDSS_NATIVE_DOMAIN;

typedef struct {
  UINT32 Width, Height, Stride, Format;
  UINT32 HFront, HSync, HBack, VFront, VSync, VBack;
  UINT64 PixelHz;
} CR_MDSS_NATIVE_MODE;

typedef struct {
  UINT32 Owner, State, Flags, Controller;
  UINT64 Dpu, InterfaceBase, PipeBase, CtlBase, MixerBase;
  UINT64 LinkBase, PhyBase, PllBase;
  UINT32 DpuBytes, InterfaceBytes, PipeBytes, CtlBytes, MixerBytes;
  UINT32 LinkBytes, PhyBytes, PllBytes;
  CR_MDSS_NATIVE_MODE Mode;
  UINT64 FrameBuffer, FrameBufferBytes, DeviceAddress;
  UINT32 MappingId, StreamCount, MappingState, MappingOwner;
  /* Raw native EFI status, zero only on successful mapping qualification.
     An uncertain/failed route never becomes transferable by zero-filling it.
     MappingOwner is SMMU only, and MappingState distinguishes a preserved
     live translation from an observed revoked range, no mapping, or unknown.
     STOPPED+REVOKED is normal, not a failed live-mapping query. */
  UINT64 MappingStatus;
  SmmuMappingEvidence Streams[CR_MDSS_NATIVE_STREAMS];
  UINT32 ClockFirst, ClockCount, RailFirst, RailCount;
  UINT32 PathFirst, PathCount, DomainFirst, DomainCount;
  UINT32 SourceRate, SourceSelect, SourceDivider, InterfaceDivider;
  UINT32 PixelSource, PixelDivider, PixelM, PixelN, PixelD, Reserved;
} CR_MDSS_NATIVE_ROUTE;

typedef struct {
  UINT32 State, Flags, I2cInstance, I2cAddress;
  UINT32 I2cRateHz, StretchUs, PowerGpio0, PowerGpio1;
  UINT32 ResetGpio, IrqGpio, HighPins, Reserved;
} CR_MDSS_NATIVE_BRIDGE_STATE;

typedef struct {
  UINT32 State, Flags, UsbOwners, DpOwners;
  UINT64 Base, Bytes;
  /* LaneCount is the successful DP link width when LinkRate is nonzero;
     otherwise the common route allocation when COMMON is ready. */
  UINT32 Orientation, LaneCount, LinkRate, Reserved;
  UINT64 Status;
  UINT32 ReadyMask, TouchedMask;
} CR_MDSS_NATIVE_PHY_STATE;

/* Native and Windows use the same little-endian scalar layout. The producer
   preallocates reserved storage and an ACPI locator before EBS. Sequence is
   odd while writing and even/nonzero after release publication; Crc32 covers
   Size bytes with Crc32 zero. FINAL requires actual retirement of every
   registered producer, including GOP, DP, bridge IRQ work, shared PHY and
   the SMMU EBS pass. Event registration order is never that proof.

   FINAL certifies immutable observations, not that all hardware stopped.
   The primary GOP can remain LIVE. Any UNCERTAIN state, unresolved retained
   error or missing independent OS memory/DMA/serial/power qualification must
   make the Windows native claim decline before touching hardware. */
typedef struct {
  UINT32 Signature, Revision, Size, Sequence;
  UINT32 Crc32, Phase, Platform, RequiredOwners;
  UINT32 RegisteredOwners, FrozenOwners, ClockCount, RailCount;
  UINT32 PathCount, DomainCount, Reserved0, Reserved1;
  UINT64 Generation, TransferId, RecordPhysical, RecordBytes;
  CR_MDSS_NATIVE_ROUTE Routes[CR_MDSS_NATIVE_ROUTES];
  CR_MDSS_NATIVE_CLOCK Clocks[CR_MDSS_NATIVE_CLOCKS];
  CR_MDSS_NATIVE_RAIL Rails[CR_MDSS_NATIVE_RAILS];
  CR_MDSS_NATIVE_PATH Paths[CR_MDSS_NATIVE_PATHS];
  CR_MDSS_NATIVE_DOMAIN Domains[CR_MDSS_NATIVE_DOMAINS];
  CR_MDSS_NATIVE_BRIDGE_STATE Bridge;
  CR_MDSS_NATIVE_PHY_STATE Phy;
} CR_MDSS_NATIVE_HANDOFF;

CR_STATIC_ASSERT(sizeof(CR_MDSS_NATIVE_PHY_STATE)==64,"MDSS PHY wire size");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_PHY_STATE,Status)==48,"MDSS PHY status offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_PHY_STATE,ReadyMask)==56,"MDSS PHY readiness offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_PHY_STATE,TouchedMask)==60,"MDSS PHY touched offset");
CR_STATIC_ASSERT(sizeof(CR_MDSS_NATIVE_ROUTE)==792,"MDSS route wire size");
CR_STATIC_ASSERT(sizeof(CR_MDSS_NATIVE_HANDOFF)==4832,"MDSS handoff wire size");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_ROUTE,Mode)==112,"MDSS mode offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_ROUTE,MappingState)==192,"MDSS mapping state offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_ROUTE,MappingStatus)==200,"MDSS mapping status offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_ROUTE,Streams)==208,"MDSS evidence offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Sequence)==12,"MDSS sequence offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Crc32)==16,"MDSS CRC offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Routes)==96,"MDSS routes offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Clocks)==1680,"MDSS clocks offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Rails)==3984,"MDSS rails offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Paths)==4304,"MDSS paths offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Domains)==4528,"MDSS domains offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Bridge)==4720,"MDSS bridge offset");
CR_STATIC_ASSERT(offsetof(CR_MDSS_NATIVE_HANDOFF,Phy)==4768,"MDSS PHY offset");
