/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#include <Library/gpio.h>

#define EFI_GPIO_CR_PROTOCOL_REVISION 0x0000000000010003

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

/* Exclusive SDCC2 pad92/211/212/213 lease. Acquire commits Owned before a
 * fallible configuration; a failed call with Owned must be released/retried.
 * Release restores the immutable original snapshot and retains ownership on
 * failure. Read returns the physical level (HDK HIGH means card present). */
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_ACQUIRE_SD)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *StableOwner, OUT BOOLEAN *Owned);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_SET_SD_ACTIVE)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *StableOwner, IN BOOLEAN Active);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_READ_SD_CD)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *StableOwner, OUT BOOLEAN *High);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_RELEASE_SD)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *StableOwner);

/* Revision1.2: exclusive ordinary-pad leases. The provider rejects SD pads,
 * enabled or retained legacy IRQ owners and conflicting consumer identities.
 * Failed setup/restore retains Owned and its immutable original snapshot.
 * Set/Read/Release require the identical stable owner used for Acquire. */
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_ACQUIRE_PIN)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *, IN CONST GpioConfigParams *, OUT BOOLEAN *);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_SET_PIN_VALUE)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *, IN UINT16, IN UINT8);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_READ_PIN_VALUE)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *, IN UINT16, OUT UINT8 *);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_RELEASE_PIN)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *, IN UINT16);
/* Revision1.3: owner-scoped raw falling-edge polling, CPU IRQ masked.
 * ReleasePin restores IRQ configuration even after failed arming. */
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_ARM_PIN_FALLING_EDGE)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *, IN UINT16);
typedef EFI_STATUS(EFIAPI *EFI_GPIO_CR_TAKE_PIN_EDGE)(
    IN EFI_GPIO_CR_PROTOCOL *, IN VOID *, IN UINT16, OUT BOOLEAN *);

typedef struct _EFI_GPIO_CR_PROTOCOL {
  UINT64                              Revision;
  EFI_GPIO_CR_READ_IO_VALUE           ReadIoValue;
  EFI_GPIO_CR_SET_IO_VALUE            SetIoValue;
  EFI_GPIO_CR_INIT_CFG_PARAM          InitGpioConfigParams;
  EFI_GPIO_CR_CONFIG_GPIO             ConfigGpio;
  EFI_GPIO_CR_GET_GPIO_CFG            GetGpioConfig;
  EFI_GPIO_CR_REGISTER_GPIO_INTERRUPT RegisterGpioInterrupt;
  EFI_GPIO_CR_ACQUIRE_SD              AcquireSd;
  EFI_GPIO_CR_SET_SD_ACTIVE           SetSdActive;
  EFI_GPIO_CR_READ_SD_CD              ReadSdCardDetect;
  EFI_GPIO_CR_RELEASE_SD              ReleaseSd;
  EFI_GPIO_CR_ACQUIRE_PIN             AcquirePin;
  EFI_GPIO_CR_SET_PIN_VALUE           SetPinValue;
  EFI_GPIO_CR_READ_PIN_VALUE          ReadPinValue;
  EFI_GPIO_CR_RELEASE_PIN             ReleasePin;
  EFI_GPIO_CR_ARM_PIN_FALLING_EDGE     ArmPinFallingEdge;
  EFI_GPIO_CR_TAKE_PIN_EDGE            TakePinEdge;
} EFI_GPIO_CR_PROTOCOL;
