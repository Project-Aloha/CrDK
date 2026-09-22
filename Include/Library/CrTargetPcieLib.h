/** @file
 *  Portable target-data ABI for Qualcomm PCIe and QMP PHY descriptions.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_interrupt.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

/* PcieIoOps is defined by pcie.h.  Keep this header independent from it so
 * target descriptions can be consumed by generators without a library
 * dependency cycle. */
struct _PcieIoOps;

typedef enum {
  PCIE_RANGE_IO,
  PCIE_RANGE_MEM32,
  PCIE_RANGE_MEM64
} PCIE_RANGE_TYPE;

typedef enum {
  PCIE_PHY_PHASE_BASE,
  PCIE_PHY_PHASE_RC
} PCIE_PHY_INIT_PHASE;

typedef enum {
  PCIE_PHY_BLOCK_SERDES,
  PCIE_PHY_BLOCK_TX,
  PCIE_PHY_BLOCK_RX,
  PCIE_PHY_BLOCK_PCS,
  PCIE_PHY_BLOCK_PCS_MISC
} PCIE_PHY_BLOCK;

typedef struct {
  CONST CHAR8 *Name;
  UINT64       Base;
  UINT64       Size;
} PcieTargetRegion;

typedef struct {
  PCIE_RANGE_TYPE Type;
  UINT64          PciBase;
  UINT64          CpuBase;
  UINT64          Size;
} PcieTargetRange;

typedef struct {
  CONST CHAR8              *Name;
  UINT32                    InterruptNumber;
  CR_INTERRUPT_TRIGGER_TYPE Trigger;
} PcieTargetInterrupt;

/**
 * Owner/provider route for a Linux clock-names entry.
 *
 * GCC entries are programmed by ClockCrDxe.  A PHY output is a fixed-rate
 * clock provider registered by the QMP PHY; its physical GCC pipe clocks are
 * acquired by the PHY phase instead of this logical output entry.  The
 * RPMh CXO reference is an always-on input and therefore has no enable
 * transaction in the PCIe host adapter.
 */
typedef enum {
  PCIE_CLOCK_PROVIDER_GCC = 0,
  PCIE_CLOCK_PROVIDER_PHY_OUTPUT_ALIAS,
  PCIE_CLOCK_PROVIDER_RPMH_ALWAYS_ON,
  PCIE_CLOCK_PROVIDER_MAX
} PCIE_CLOCK_PROVIDER;

typedef struct {
  CONST CHAR8 *Name;
  CONST CHAR8 *Controller;
  CONST CHAR8 *Id;
  UINT32       RateHz;
  PCIE_CLOCK_PROVIDER Provider;
} PcieTargetClock;

typedef PcieTargetClock PcieTargetReset;

/**
 * A regulator vote owned by the platform RPMh/regulator driver.
 *
 * The PCIe library does not program regulator registers itself.  The target
 * data only carries the Linux-style resource identity and the optional
 * operating point which the host-bridge adapter can pass to the existing
 * regulator implementation.  A zero voltage/load means that the board
 * default is retained.
 */
typedef struct {
  CONST CHAR8 *Name;
  CONST CHAR8 *Controller;
  CONST CHAR8 *Id;
  UINT32       VoltageMv;
  UINT32       LoadUa;
} PcieTargetSupply;

typedef struct {
  CONST CHAR8 *Role;
  CONST CHAR8 *Controller;
  UINT16       Pin;
  CONST CHAR8 *Polarity;
} PcieTargetGpio;

typedef struct {
  UINT16       Bdf;
  CONST CHAR8 *Controller;
  UINT32       StreamId;
  UINT16       Count;
} PcieTargetIommuMap;

/**
 * Interconnect paths consumed by one root complex.
 *
 * Qualcomm DTs describe separate memory and CPU/config paths.  Keeping the
 * endpoint IDs in target data lets the generic PCIe library hand them to the
 * interconnect owner without embedding SM8450 IDs in PciHostBridgeLib.
 */
typedef struct {
  CONST CHAR8 *Provider;
  UINT32       MemSource;
  UINT32       MemDestination;
  UINT32       CpuSource;
  UINT32       CpuDestination;
  UINT64       MemAverage;
  UINT64       MemPeak;
  UINT64       CpuAverage;
  UINT64       CpuPeak;
} PcieTargetInterconnect;

typedef struct {
  PCIE_PHY_INIT_PHASE Phase;
  PCIE_PHY_BLOCK      Block;
  UINT32              Offset;
  UINT32              Value;
  UINT8               LaneMask;
} PciePhyInitEntry;

typedef struct {
  UINT32 Serdes;
  UINT32 Tx;
  UINT32 Rx;
  UINT32 Tx2;
  UINT32 Rx2;
  UINT32 Pcs;
  UINT32 PcsMisc;
} PciePhyBlockOffsets;

typedef struct {
  CONST CHAR8        *Label;
  CONST CHAR8        *Compatible;
  UINT64              Base;
  UINT64              Size;
  UINT8               Lanes;
  PciePhyBlockOffsets BlockOffsets;
  UINT32              SwReset;
  UINT32              StartControl;
  UINT32              Status;
  UINT32              PowerDownControl;
  UINT32              PowerDownControlValue;
  UINT32              StatusMask;
  UINT16              InitOffset;
  UINT16              InitCount;
  UINT16              ClockOffset;
  UINT16              ClockCount;
  UINT16              ResetOffset;
  UINT16              ResetCount;
} PcieTargetPhy;

typedef struct {
  CONST CHAR8 *Label;
  CONST CHAR8 *Compatible;
  CONST CHAR8 *LinuxConfig;
  CONST CHAR8 *LinuxOps;
  BOOLEAN      Enabled;
  UINT16       Domain;
  UINT8        BusStart;
  UINT8        BusEnd;
  UINT8        Lanes;
  UINT16       RegionOffset;
  UINT16       RegionCount;
  UINT16       RangeOffset;
  UINT16       RangeCount;
  UINT16       InterruptOffset;
  UINT16       InterruptCount;
  UINT16       IntxOffset;
  UINT16       IntxCount;
  UINT16       ClockOffset;
  UINT16       ClockCount;
  UINT16       ResetOffset;
  UINT16       ResetCount;
  UINT16       SupplyOffset;
  UINT16       SupplyCount;
  CONST CHAR8 *PowerDomainController;
  CONST CHAR8 *PowerDomainId;
  UINT16       GpioOffset;
  UINT16       GpioCount;
  UINT16       IommuMapOffset;
  UINT16       IommuMapCount;
  UINT16       PhyIndex;
  PcieTargetInterconnect Interconnect;
} PcieTargetController;

typedef struct {
  CONST PcieTargetController *Controllers;
  UINT16                      ControllerCount;
  CONST PcieTargetRegion     *Regions;
  UINT16                      RegionCount;
  CONST PcieTargetRange      *Ranges;
  UINT16                      RangeCount;
  CONST PcieTargetInterrupt  *Interrupts;
  UINT16                      InterruptCount;
  CONST PcieTargetClock      *Clocks;
  UINT16                      ClockCount;
  CONST PcieTargetReset      *Resets;
  UINT16                      ResetCount;
  CONST PcieTargetSupply     *Supplies;
  UINT16                      SupplyCount;
  CONST PcieTargetGpio       *Gpios;
  UINT16                      GpioCount;
  CONST PcieTargetIommuMap   *IommuMaps;
  UINT16                      IommuMapCount;
  CONST PcieTargetPhy        *Phys;
  UINT16                      PhyCount;
  CONST PciePhyInitEntry     *PhyInit;
  UINT16                      PhyInitCount;
} PcieTargetContext;

PcieTargetContext *
CrTargetGetPcieContext(VOID);

/**
  Return the platform callbacks used by the cold PCIe initializer.

  The target implementation must route each resource to its owning driver;
  this is deliberately a callback table instead of a private PCIe DXE
  protocol.  The opaque tag is completed by Include/Library/pcie.h.
**/
CR_STATUS
CrTargetGetPcieIo(
  OUT struct _PcieIoOps *Io
  );
