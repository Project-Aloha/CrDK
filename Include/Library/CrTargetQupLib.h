/** @file
 *  QUPv3/GENI and GPI target description.
 *
 *  The descriptor mirrors the Linux device-tree resources.  GpiCrDxe owns the
 *  GPI-DMA registers for the explicitly supported I2C endpoint.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Uefi.h>
#include <Library/gpio.h>

#define CR_TARGET_QUP_MAX_SERIAL_ENGINES  8
#define CR_TARGET_GPI_MAX_INTERRUPTS      12
#define CR_TARGET_QUP_ICC_PATHS            3
#define CR_TARGET_QUP_MAX_I2C_ENDPOINTS   32

/* Linux GENI core, config and memory paths, in resource-enable order.
   Bandwidths are kB/s; each entry covers the supported SE operating range. */
typedef struct {
  CONST CHAR8 *Provider;
  UINT32 Source;
  UINT32 Destination;
  UINT32 Bandwidth;
} CR_TARGET_QUP_ICC_PATH;

typedef struct {
  UINT8  Id;
  UINT64 Base;
  UINT32 Size;
  UINT8  SerialEngineCount;
  UINT64 SerialEngineBase[CR_TARGET_QUP_MAX_SERIAL_ENGINES];
  UINT32 SerialEngineSize;
  UINT32 IommuStreamId;
  CONST CR_TARGET_QUP_ICC_PATH *SerialEngineInterconnect[CR_TARGET_QUP_MAX_SERIAL_ENGINES];
} CR_TARGET_QUP_WRAPPER;

typedef struct {
  UINT8  Id;
  UINT8  WrapperId;
  UINT64 Base;
  UINT32 Size;
  UINT32 ChannelMask;
  UINT32 IommuStreamId;
  UINT32 InterruptCount;
  UINT32 Interrupts[CR_TARGET_GPI_MAX_INTERRUPTS];
} CR_TARGET_GPI_CONTROLLER;

typedef struct {
  UINT16 Pin;
  GpioFunctionType Function;
  GpioPullType Pull;
  GpioDriveStrength DriveStrength;
} CR_TARGET_QUP_I2C_PIN;

/* Board-selected FIFO buses. Public Instance IDs need not equal SE numbers.
   Base/Size must exactly identify the wrapper's SE view. Pin mux is always
   peripheral input (OE=0); the GPIO output latch is preserved. SourceClockHz
   must match the driver's supported timing table, not an inferred rate. */
typedef struct {
  UINT32 Instance;
  UINT8 Wrapper, Engine;
  UINT64 Base;
  UINT32 Size;
  CR_TARGET_QUP_I2C_PIN Pins[2];
  CONST CHAR8 *ClockController;
  CONST CHAR8 *Clocks[3]; /* mAHB, sAHB, SE, in acquisition order. */
  UINT32 SourceClockHz, DefaultFrequencyKHz;
} CR_TARGET_QUP_I2C_ENDPOINT;

typedef struct {
  CONST CR_TARGET_QUP_WRAPPER *Qup;
  UINT8                       QupCount;
  CONST CR_TARGET_GPI_CONTROLLER *Gpi;
  UINT8                       GpiCount;
  /* Appended for QUP protocol1.2. CrDAL's DataSize guard protects consumers
     against an older target. Immutable storage valid until ExitBootServices. */
  CONST CR_TARGET_QUP_I2C_ENDPOINT *I2c;
  UINT32                           I2cCount;
} CR_TARGET_QUP_CONTEXT;

CR_TARGET_QUP_CONTEXT *
CrTargetGetQupContext (
  VOID
  );
