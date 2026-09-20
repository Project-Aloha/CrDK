/** @file
 *  OS-independent Qualcomm PCIe root-complex and QMP PHY support.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetPcieLib.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

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
typedef CR_STATUS (*PCIE_SET_GPIO)(
    IN VOID *Context, IN CONST PcieTargetGpio *Gpio,
    IN PCIE_GPIO_DIRECTION Direction, IN BOOLEAN Active);

typedef struct {
  PCIE_MMIO_READ32        Read32;
  PCIE_MMIO_WRITE32       Write32;
  PCIE_DELAY_US           DelayUs;
  PCIE_SET_CLOCK          SetClock;
  PCIE_SET_RESET          SetReset;
  PCIE_SET_POWER_DOMAIN   SetPowerDomain;
  PCIE_SET_GPIO           SetGpio;
  VOID                   *Context;
} PcieIoOps;

typedef struct {
  CONST PcieTargetController *Target;
  CONST PcieTargetPhy        *Phy;
  UINT64                      ParfBase;
  UINT64                      DbiBase;
  UINT64                      AtuBase;
  UINT64                      ConfigBase;
  UINT64                      ConfigSize;
  CR_STATUS                   LastStatus;
  PCIE_PORT_STATE             State;
  BOOLEAN                     GpiosConfigured;
  BOOLEAN                     PowerEnabled;
  BOOLEAN                     ControllerClocksEnabled;
  BOOLEAN                     ControllerResetsManaged;
  BOOLEAN                     PhyClocksEnabled;
  BOOLEAN                     PhyResetsManaged;
} PciePortRuntime;

typedef struct {
  CONST PcieTargetContext *Target;
  PcieIoOps                Io;
  PciePortRuntime          Ports[PCIE_MAX_CONTROLLERS];
  UINT32                   InitializedMask;
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

/** Return runtime and generated information for a port. */
CR_STATUS
PcieGetPortInfo(
    IN PcieDeviceContext *Context, IN UINT16 PortIndex,
    OUT CONST PciePortRuntime **Port);
