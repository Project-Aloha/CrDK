/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>

#include <Library/gpio.h>
#include <Library/pdc.h>
#include <oskal/cr_debug.h>

#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>

STATIC GpioDeviceContext *mGpioContext = NULL;

/* Protocol wrapper implementation */
EFI_STATUS
EFIAPI ProtocolGpioReadInputValue(
    IN EFI_GPIO_CR_PROTOCOL *This, IN UINT16 GpioIndex, OUT UINT8 *Value)
{
  if (This == NULL || Value == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Value = (UINT8)GpioReadInputVal(mGpioContext, GpioIndex);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI ProtocolGpioSetIOValue(
    IN EFI_GPIO_CR_PROTOCOL *This, IN UINT16 GpioIndex, IN UINT8 Value)
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  // Construct ConfigParams
  GpioConfigParams ConfigParams = {0};
  GpioInitConfigParams(&ConfigParams, GpioIndex);
  ConfigParams.OutputEnable = TRUE;
  ConfigParams.OutputValue  = (Value != 0) ? GPIO_VALUE_HIGH : GPIO_VALUE_LOW;
  GpioSetConfig(mGpioContext, &ConfigParams);

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI ProtocolGpioInitConfigParams(
    IN EFI_GPIO_CR_PROTOCOL *This, IN OUT GpioConfigParams *ConfigParams)
{
  if (This == NULL || ConfigParams == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  GpioInitConfigParams(ConfigParams, ConfigParams->PinNumber);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI ProtocolGpioConfig(
    IN EFI_GPIO_CR_PROTOCOL *This, IN GpioConfigParams *ConfigParams)
{
  if (This == NULL || ConfigParams == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  GpioSetConfig(mGpioContext, ConfigParams);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI ProtocolGpioGetConfig(
    IN EFI_GPIO_CR_PROTOCOL *This, IN OUT GpioConfigParams *ConfigParams)
{
  if (This == NULL || ConfigParams == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  GpioReadConfig(mGpioContext, ConfigParams);
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI ProtocolGpioRegisterGpioInterrupt(
    IN EFI_GPIO_CR_PROTOCOL *This, IN UINT16 GpioIndex,
    IN CR_INTERRUPT_HANDLER GpioIrqHandler, IN VOID *HandlerParam,
    IN CR_INTERRUPT_TRIGGER_TYPE TriggerType)
{
  CR_STATUS Status = CR_SUCCESS;

  if (This == NULL || GpioIrqHandler == NULL ||
      GpioIndex >= mGpioContext->PinCount) {
    return EFI_INVALID_PARAMETER;
  }

  // Register interrupt
  Status = GpioRegisterGpioIrq(
      mGpioContext, GpioIndex, GpioIrqHandler, HandlerParam, TriggerType);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": Failed to register interrupt for GPIO %d, Status=0x%X",
        __FUNCTION__, GpioIndex, Status);
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_GPIO_CR_PROTOCOL gGpioCrProtocol = {
    .Revision             = EFI_GPIO_CR_PROTOCOL_REVISION,
    .ReadIoValue          = ProtocolGpioReadInputValue,
    .SetIoValue           = ProtocolGpioSetIOValue,
    .InitGpioConfigParams = ProtocolGpioInitConfigParams,
    .ConfigGpio           = ProtocolGpioConfig,
    .GetGpioConfig        = ProtocolGpioGetConfig,
    .RegisterGpioInterrupt = ProtocolGpioRegisterGpioInterrupt,
};

// For test use.
// VOID SimpleGpioIrqHandler(VOID *Param)
// {
//   UINT16 GpioIndex = *(UINT16 *)Param;
//   log_info("SimpleGpioIrqHandler: GPIO %d Interrupt Triggered!",
//   GpioIndex);
// }

CR_STATUS
GpioEntryPoint(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // Init PDC
  PdcDeviceContext *PdcCtx = NULL;
  if (CR_ERROR(PdcLibInit(&PdcCtx)) || (PdcCtx == NULL)) {
    log_err("PdcLibInit failed");
    return EFI_DEVICE_ERROR;
  }

  // Init Gpio
  if (CR_ERROR(GpioLibInit(&mGpioContext)) || (mGpioContext == NULL)) {
    log_err("GpioLibInit failed");
    return EFI_DEVICE_ERROR;
  }

  // Install protocol
  Status = gBS->InstallMultipleProtocolInterfaces(
      &ImageHandle, &gEfiGpioCrProtocolGuid, &gGpioCrProtocol, NULL);
  if (EFI_ERROR(Status)) {
    log_err("Failed to install Gpio CR Protocol, Status=0x%X", Status);
    return Status;
  }

  // Test case (on hdk8450)
#if 0
  {
  // GpioConfigParams ConfigParams = {0};
  // GpioInitConfigParams(&ConfigParams, 8);
  // Test Gpio8 Blink
  // ConfigParams.OutputEnable  = TRUE;
  // ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  // ConfigParams.Pull          = GPIO_PULL_NONE;
  // ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  // ConfigParams.OutputValue   = GPIO_VALUE_HIGH;

  // while (1) {
  //   log_info("Set Gpio8 to %d", ConfigParams.OutputValue);
  //   GpioSetConfig(GpioContext, &ConfigParams);
  //   cr_sleep(1 * 1000 * 1000); // 1s
  //   ConfigParams.OutputValue = !ConfigParams.OutputValue;
  // }

  // Test Gpio13 Input Read
  // ConfigParams.PinNumber     = 13;
  // ConfigParams.OutputEnable  = FALSE;
  // ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  // ConfigParams.Pull          = GPIO_PULL_UP;
  // ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  // GpioSetConfig(GpioContext, &ConfigParams);

  // while (1) {
  //   GpioValueType Val = GpioReadInputVal(GpioContext, 13);
  //   log_info("Gpio13 Input Value: %d", Val);
  //   cr_sleep(100 * 1000); // 0.1s
  // }

  // {
  //   // Test Gpio48 Interrupt
  //   // 1. Set Gpio13 Output Low
  //   GpioInitConfigParams(&ConfigParams, 13);
  //   ConfigParams.PinNumber     = 13;
  //   ConfigParams.OutputEnable  = TRUE;
  //   ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  //   ConfigParams.Pull          = GPIO_PULL_NONE;
  //   ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  //   ConfigParams.OutputValue   = GPIO_VALUE_LOW;
  //   GpioSetConfig(GpioContext, &ConfigParams);
  //   log_info("Gpio13 configured as Output Low.");

  //   // 2. Set Gpio48 Input with Pull Down
  //   GpioInitConfigParams(&ConfigParams, 48);
  //   ConfigParams.PinNumber     = 48;
  //   ConfigParams.OutputEnable  = FALSE;
  //   ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  //   ConfigParams.Pull          = GPIO_PULL_DOWN;
  //   ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  //   GpioSetConfig(GpioContext, &ConfigParams);
  //   log_info("Gpio48 configured as Input with Pull Down.");

  //   // 3. Register Interrupt on Gpio48 lvl trigger
  //   STATIC UINT16 GpioIndex = 48;
  //   GpioRegisterGpioIrq(
  //       GpioContext, GpioIndex, SimpleGpioIrqHandler, &GpioIndex,
  //       CR_INTERRUPT_TRIGGER_LEVEL_HIGH);

  //   // Blink Gpio13 to trigger Gpio48 interrupt
  //   GpioInitConfigParams(&ConfigParams, 13);
  //   ConfigParams.PinNumber     = 13;
  //   ConfigParams.OutputEnable  = TRUE;
  //   ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  //   ConfigParams.Pull          = GPIO_PULL_NONE;
  //   ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  //   ConfigParams.OutputValue   = GPIO_VALUE_HIGH;
  //   while (1) {
  //     cr_sleep(1 * 1000 * 1000); // 1s
  //     log_info("Set Gpio13 to %d", ConfigParams.OutputValue);
  //     GpioSetConfig(GpioContext, &ConfigParams);
  //     ConfigParams.OutputValue = !ConfigParams.OutputValue;
  //   }
  // }
  // {
  //   // Test Gpio63 Pdc Interrupt
  //   // 1. Set Gpio13 Output Low
  //   GpioInitConfigParams(&ConfigParams, 13);
  //   ConfigParams.PinNumber     = 13;
  //   ConfigParams.OutputEnable  = TRUE;
  //   ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  //   ConfigParams.Pull          = GPIO_PULL_NONE;
  //   ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  //   ConfigParams.OutputValue   = GPIO_VALUE_LOW;
  //   GpioSetConfig(GpioContext, &ConfigParams);
  //   log_info("Gpio13 configured as Output Low.");

  //   // 2. Set Gpio63 Input with Pull NONE
  //   GpioInitConfigParams(&ConfigParams, 63);
  //   ConfigParams.PinNumber     = 63;
  //   ConfigParams.OutputEnable  = FALSE;
  //   ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  //   ConfigParams.Pull          = GPIO_PULL_NONE;
  //   ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  //   GpioSetConfig(GpioContext, &ConfigParams);
  //   log_info("Gpio63 configured as Input with Pull DOWN.");

  //   // 3. Register Interrupt on Gpio63 edge trigger
  //   STATIC UINT16 GpioIndex = 63;
  //   GpioRegisterGpioIrq(
  //       GpioContext, GpioIndex, SimpleGpioIrqHandler, &GpioIndex,
  //       CR_INTERRUPT_TRIGGER_EDGE_BOTH);

  //   // Blink Gpio13 to trigger Gpio63 interrupt
  //   GpioInitConfigParams(&ConfigParams, 13);
  //   ConfigParams.PinNumber     = 13;
  //   ConfigParams.OutputEnable  = TRUE;
  //   ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
  //   ConfigParams.Pull          = GPIO_PULL_NONE;
  //   ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;
  //   ConfigParams.OutputValue   = GPIO_VALUE_HIGH;
  //   while (1) {
  //     cr_sleep(1 * 1000 * 1000); // 1s
  //     log_info("Set Gpio13 to %d", ConfigParams.OutputValue);
  //     GpioSetConfig(GpioContext, &ConfigParams);
  //     ConfigParams.OutputValue = !ConfigParams.OutputValue;
  //   }
  // }
  }
#endif

// Protocol test cases (hdk8450)
#if 0
  {
    EFI_GPIO_CR_PROTOCOL *GpioProtocol = NULL;
    GpioConfigParams      ConfigParams = {0};
    UINT16                Gpio4Test    = 8;

    Status = gBS->LocateProtocol(
        &gEfiGpioCrProtocolGuid, NULL, (VOID **)&GpioProtocol);
    if (EFI_ERROR(Status) || GpioProtocol == NULL) {
      log_err("Failed to locate Gpio CR Protocol, Status=0x%X", Status);
      return Status;
    }

    // Init Config Params
    Status = GpioProtocol->InitGpioConfigParams(GpioProtocol, &ConfigParams);
    if (EFI_ERROR(Status)) {
      log_err("InitGpioConfigParams failed, Status=0x%X", Status);
      return Status;
    }

    // Blink 3 times
    ConfigParams.PinNumber     = Gpio4Test;
    ConfigParams.OutputEnable  = TRUE;
    ConfigParams.OutputValue   = GPIO_VALUE_HIGH;
    ConfigParams.FunctionSel   = GPIO_FUNC_NORMAL;
    ConfigParams.DriveStrength = GPIO_DRIVE_STRENGTH_8MA;

    for (UINT8 i = 0; i < 3 * 2; i++) {
      log_info(
          "Protocol: Set Gpio%d to %d", Gpio4Test, ConfigParams.OutputValue);
      Status = GpioProtocol->ConfigGpio(GpioProtocol, &ConfigParams);
      if (EFI_ERROR(Status)) {
        log_err("ConfigGpio failed, Status=0x%X", Status);
        return Status;
      }
      ConfigParams.OutputValue = !ConfigParams.OutputValue;
      cr_sleep(500 * 1000); // 0.5s
    }
  }
#endif

  return EFI_SUCCESS;
}