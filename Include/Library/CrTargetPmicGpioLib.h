/** @file
 *  Portable target-data ABI for SPMI PMIC GPIO controllers.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_types.h>

typedef struct {
  CONST CHAR8 *Name;
  CONST CHAR8 *Compatible;
  UINT8        Sid;
  UINT16       PeripheralBase;
  UINT16       PinCount;
} PmicGpioTargetController;

typedef struct {
  CONST PmicGpioTargetController *Controllers;
  UINT16                          ControllerCount;
} PmicGpioTargetContext;

PmicGpioTargetContext *
CrTargetGetPmicGpioContext(VOID);
