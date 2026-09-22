/** @file
 *  OS-independent Qualcomm PCIe root-complex and QMP PHY support.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetPcieLib.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <oskal/cr_lock.h>

#define PCIE_MAX_CONTROLLERS 8

typedef enum {
  PCIE_GPIO_INPUT,
  PCIE_GPIO_OUTPUT
} PCIE_GPIO_DIRECTION;

typedef enum {
  PCIE_PORT_OFF,
  PCIE_PORT_INITIALIZING,
  PCIE_PORT_READY_NO_LINK,
  PCIE_PORT_LINK_UP,
  PCIE_PORT_FAILED
} PCIE_PORT_STATE;

typedef UINT32 (*PCIE_MMIO_READ32)(IN VOID *Context, IN UINT64 Address);
typedef VOID (*PCIE_MMIO_WRITE32)(
    IN VOID *Context, IN UINT64 Address, IN UINT32 Value);
typedef VOID (*PCIE_DELAY_US)(IN VOID *Context, IN UINT32 Microseconds);
typedef CR_STATUS (*PCIE_SET_CLOCK)(
    IN VOID *Context, IN CONST PcieTargetClock *Clock, IN BOOLEAN Enable);
typedef CR_STATUS (*PCIE_SET_RESET)(
    IN VOID *Context, IN CONST PcieTargetReset *Reset, IN BOOLEAN Assert);
typedef CR_STATUS (*PCIE_SET_POWER_DOMAIN)(
    IN VOID *Context, IN CONST PcieTargetController *Controller,
    IN BOOLEAN Enable);
/** Enable or release one regulator vote owned by the platform PMIC driver. */
typedef CR_STATUS (*PCIE_SET_SUPPLY)(
    IN VOID *Context, IN CONST PcieTargetSupply *Supply,
    IN BOOLEAN Enable);
/** Vote the two RPMh interconnect paths required by a root complex. */
typedef CR_STATUS (*PCIE_SET_INTERCONNECT)(
    IN VOID *Context, IN CONST PcieTargetController *Controller,
    IN BOOLEAN Enable);
/** Attach/block all BDF stream IDs before/after the root port is exposed. */
typedef CR_STATUS (*PCIE_SET_IOMMU)(
    IN VOID *Context, IN CONST PcieTargetController *Controller,
    IN BOOLEAN Enable);
typedef CR_STATUS (*PCIE_SET_GPIO)(
    IN VOID *Context, IN CONST PcieTargetGpio *Gpio,
    IN PCIE_GPIO_DIRECTION Direction, IN BOOLEAN Active);

typedef struct _PcieIoOps {
  PCIE_MMIO_READ32        Read32;
  PCIE_MMIO_WRITE32       Write32;
  PCIE_DELAY_US           DelayUs;
  PCIE_SET_CLOCK          SetClock;
  PCIE_SET_RESET          SetReset;
  PCIE_SET_POWER_DOMAIN   SetPowerDomain;
  PCIE_SET_SUPPLY         SetSupply;
  PCIE_SET_INTERCONNECT   SetInterconnect;
  PCIE_SET_IOMMU          SetIommu;
  PCIE_SET_GPIO           SetGpio;
  VOID                   *Context;
} PcieIoOps;

typedef struct {
  CONST PcieTargetController *Target;
  CONST PcieTargetPhy        *Phy;
  UINT64                      ParfBase;
  UINT64                      DbiBase;
  UINT64                      DbiSize;
  UINT64                      AtuBase;
  UINT64                      ConfigBase;
  UINT64                      ConfigSize;
  CR_STATUS                   LastStatus;
  PCIE_PORT_STATE             State;
  BOOLEAN                     GpiosConfigured;
  BOOLEAN                     SuppliesEnabled;
  BOOLEAN                     PowerEnabled;
  BOOLEAN                     ControllerClocksEnabled;
  BOOLEAN                     ControllerResetsManaged;
  BOOLEAN                     PhyClocksEnabled;
  BOOLEAN                     PhyResetsManaged;
  BOOLEAN                     InterconnectEnabled;
  BOOLEAN                     IommuAttached;
} PciePortRuntime;

typedef struct {
  CONST PcieTargetContext *Target;
  PcieIoOps                Io;
  PciePortRuntime          Ports[PCIE_MAX_CONTROLLERS];
  CR_LOCK                  ConfigLock;
  UINT32                   InitializedMask;
  /* Ports whose cold-init resources are still owned, including a failed
   * teardown which must be retried before the context can be discarded. */
  UINT32                   ResourceMask;
  UINT32                   LinkMask;
  BOOLEAN                  Initialized;
} PcieDeviceContext;

/** Validate target data and initialize a reusable device context. */
CR_STATUS
PcieLibInit(
    OUT PcieDeviceContext **Context, IN CONST PcieTargetContext *Target,
    IN CONST PcieIoOps *Io);

/** Initialize one generated root-complex port. Link-down is not an error. */
CR_STATUS
PcieInitializePort(IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex);

/** Initialize every port selected by PortMask and return successful ports. */
CR_STATUS
PcieInitializeMask(
    IN OUT PcieDeviceContext *Context, IN UINT32 PortMask,
    OUT UINT32 *InitializedMask);

/** Assert PERST and release resources acquired for a port. */
CR_STATUS
PcieShutdownPort(IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex);

/** Re-read the root-port link state. */
BOOLEAN
PcieIsLinkUp(IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex);

/**
  Access one configuration register through the DesignWare outbound CFG
  window.  Root-port BDF 0:0.0 is read directly from DBI; root-bus slots and
  functions other than 0:0.0 are absent, while downstream BDFs reprogram iATU
  region 0 while ConfigLock is held.
**/
CR_STATUS
PcieConfigAccess(
    IN OUT PcieDeviceContext *Context, IN UINT16 PortIndex, IN UINT8 Bus,
    IN UINT8 Device, IN UINT8 Function, IN UINT16 Register, IN UINTN Width,
    IN BOOLEAN Write, IN OUT UINT32 *Value);

/** Return runtime and generated information for a port. */
CR_STATUS
PcieGetPortInfo(
    IN PcieDeviceContext *Context, IN UINT16 PortIndex,
    OUT CONST PciePortRuntime **Port);
