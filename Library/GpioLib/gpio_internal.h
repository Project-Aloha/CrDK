/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once
#include <Library/gpio.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>
#include <Library/CrTargetGpioLib.h>

#include <oskal/cr_interrupt.h>

CR_STATUS
GpioInitIrq(GpioDeviceContext *Context);

VOID GpioCleanIrqStatus(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex);

STATIC inline UINTN GpioGetCfgRegByIndex(IN GpioDeviceContext *GpioContext,
                                  IN GpioConfigRegType RegType,
                                  IN UINT16 GpioIndex) {
  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    return 0;
  }

  GpioConfigRegInfo *PinRegCfg = GpioContext->GpioPins[GpioIndex].pRegCfg;

  UINTN PinAddress = GpioContext->GpioPins[GpioIndex].PinAddress;

  switch (RegType) {
  case GPIO_CFG_REG_TYPE_CTL_REG:
    if (PinRegCfg->Index == 0)
      return PinAddress;
    else
      return PinAddress + PinRegCfg->ControlRegOffset;
  case GPIO_CFG_REG_TYPE_IO_REG:
    return PinAddress + PinRegCfg->IoRegOffset;
  case GPIO_CFG_REG_TYPE_INT_CFG_REG:
    return PinAddress + PinRegCfg->IntCfgRegOffset;
  case GPIO_CFG_REG_TYPE_INT_STATUS_REG:
    return PinAddress + PinRegCfg->IntStatusRegOffset;
  case GPIO_CFG_REG_TYPE_INT_TARGET_REG:
    return PinAddress + PinRegCfg->IntTargetRegOffset;
  default:
    return 0;
  }
}
