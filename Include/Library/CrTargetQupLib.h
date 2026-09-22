/** @file
 *  SM8450 QUPv3/GENI and GPI target description.
 *
 *  The descriptor mirrors the Linux device-tree resources.  It is metadata
 * only: the MU QUP/GPI binaries remain the owners of controller programming.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Uefi.h>

#define CR_TARGET_QUP_MAX_SERIAL_ENGINES  8
#define CR_TARGET_GPI_MAX_INTERRUPTS      12

typedef struct {
  UINT8  Id;
  UINT64 Base;
  UINT32 Size;
  UINT8  SerialEngineCount;
  UINT64 SerialEngineBase[CR_TARGET_QUP_MAX_SERIAL_ENGINES];
  UINT32 SerialEngineSize;
  UINT32 IommuStreamId;
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
  CONST CR_TARGET_QUP_WRAPPER *Qup;
  UINT8                       QupCount;
  CONST CR_TARGET_GPI_CONTROLLER *Gpi;
  UINT8                       GpiCount;
} CR_TARGET_QUP_CONTEXT;

CR_TARGET_QUP_CONTEXT *
CrTargetGetQupContext (
  VOID
  );
