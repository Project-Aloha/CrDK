/** @file
 *  Portable target-data ABI for Qualcomm PCIe and QMP PHY descriptions.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_interrupt.h>
#include <oskal/cr_types.h>

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

typedef struct {
  CONST CHAR8 *Name;
  CONST CHAR8 *Controller;
  CONST CHAR8 *Id;
  UINT32       RateHz;
} PcieTargetClock;

typedef PcieTargetClock PcieTargetReset;

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
  CONST CHAR8 *PowerDomainController;
  CONST CHAR8 *PowerDomainId;
  UINT16       GpioOffset;
  UINT16       GpioCount;
  UINT16       IommuMapOffset;
  UINT16       IommuMapCount;
  UINT16       PhyIndex;
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
