/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#include <Library/gpio.h>

#define EFI_GPIO_CR_PROTOCOL_REVISION 0x0000000000010000

#define EFI_GPIO_CR_PROTOCOL_GUID                                              \
  {0x05726725, 0x2ef4, 0x4f4c, {0x99, 0xa5, 0xb9, 0x5d, 0x58, 0x19, 0x92, 0xa1}}

extern EFI_GUID                      gEfiGpioCrProtocolGuid;
typedef struct _EFI_GPIO_CR_PROTOCOL EFI_GPIO_CR_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_READ_IO_VALUE)(
    IN EFI_GPIO_CR_PROTOCOL *This, IN UINT16 GpioIndex, OUT UINT8 *Value);

typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_SET_IO_VALUE)(
    IN EFI_GPIO_CR_PROTOCOL *This, IN UINT16 GpioIndex, IN UINT8 Value);

typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_INIT_CFG_PARAM)(
    IN EFI_GPIO_CR_PROTOCOL *This, IN OUT GpioConfigParams *ConfigParams);

typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_CONFIG_GPIO)(
    IN EFI_GPIO_CR_PROTOCOL *This, IN GpioConfigParams *ConfigParams);

typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_GET_GPIO_CFG)(
    IN EFI_GPIO_CR_PROTOCOL *This, IN OUT GpioConfigParams *ConfigParams);

typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_REGISTER_GPIO_INTERRUPT)(
    IN EFI_GPIO_CR_PROTOCOL *This, IN UINT16 GpioIndex,
    IN CR_INTERRUPT_HANDLER GpioIrqHandler, IN VOID *HandlerParam,
    IN CR_INTERRUPT_TRIGGER_TYPE TriggerType);

typedef struct _EFI_GPIO_CR_PROTOCOL {
  UINT64                              Revision;
  EFI_GPIO_CR_READ_IO_VALUE           ReadIoValue;
  EFI_GPIO_CR_SET_IO_VALUE            SetIoValue;
  EFI_GPIO_CR_INIT_CFG_PARAM          InitGpioConfigParams;
  EFI_GPIO_CR_CONFIG_GPIO             ConfigGpio;
  EFI_GPIO_CR_GET_GPIO_CFG            GetGpioConfig;
  EFI_GPIO_CR_REGISTER_GPIO_INTERRUPT RegisterGpioInterrupt;
} EFI_GPIO_CR_PROTOCOL;
