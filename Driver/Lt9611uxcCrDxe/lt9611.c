#include <Library/DebugLib.h>
#include <Library/CrDalLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Uefi.h>

#include <Protocol/EFIGpioCrProtocol.h>

VOID Lt9611IrqHandler(VOID *Param)
{
  (VOID) Param;
  DEBUG((DEBUG_INFO, "LT9611Uxc Interrupt Triggered!\n"));
}

EFI_STATUS
Lt9611EntryPoint(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
  // Setup gpio
  EFI_STATUS            Status       = EFI_SUCCESS;
  EFI_GPIO_CR_PROTOCOL *GpioProtocol = NULL;
  CONST CR_DAL_LT9611_CONFIG *Config = CrDalGetLt9611Config ();

  if (Config == NULL) {
    return EFI_NOT_FOUND;
  }
  if ((Config->PowerPinCount == 0) || (Config->PowerPins == NULL)) {
    return EFI_COMPROMISED_DATA;
  }

  Status = gBS->LocateProtocol(
      &gEfiGpioCrProtocolGuid, NULL, (VOID **)&GpioProtocol);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to locate Gpio CR Protocol: %r\n", Status));
    return Status;
  }

  // Configure GPIOs for LT9611Uxc
  // Setup Interrupt
  UINT16 GpioIntPin = Config->InterruptPin;
  GpioConfigParams GpioIntConfig = {0};
  GpioIntConfig.PinNumber        = GpioIntPin;
  GpioIntConfig.OutputEnable     = FALSE; // Input
  GpioIntConfig.FunctionSel      = GPIO_FUNC_NORMAL;
  GpioIntConfig.Pull             = GPIO_PULL_NONE;
  Status = GpioProtocol->ConfigGpio(GpioProtocol, &GpioIntConfig);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (DEBUG_ERROR,
         "Failed to configure Interrupt GPIO %d for LT9611Uxc: %r\n",
         GpioIntPin, Status));
    return Status;
  }

  // Enable Power
  for (UINTN i = 0; i < Config->PowerPinCount; i++) {
    UINT16 GpioPin = Config->PowerPins[i];
    GpioConfigParams PowerConfig = {
      .PinNumber = GpioPin,
      .OutputEnable = TRUE,
      .FunctionSel = GPIO_FUNC_NORMAL,
      .Pull = GPIO_PULL_NONE,
      .DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE,
      .OutputValue = GPIO_VALUE_HIGH,
    };

    Status = GpioProtocol->ConfigGpio (GpioProtocol, &PowerConfig);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (DEBUG_ERROR, "Failed to set GPIO %d High for LT9611Uxc: %r\n",
           GpioPin, Status));
      return Status;
    }
  }

  // Register gpio interrupt
  Status = GpioProtocol->RegisterGpioInterrupt(
      GpioProtocol, GpioIntPin, Lt9611IrqHandler, NULL,
      CR_INTERRUPT_TRIGGER_EDGE_FALLING);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (DEBUG_ERROR, "Failed to register Interrupt GPIO %d for LT9611Uxc: %r\n",
                          GpioIntPin, Status));
    return Status;
  }

  return EFI_SUCCESS;
}
