#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Uefi.h>

#include <Protocol/EFIGpioCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>

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
  EFI_RPMH_CR_PROTOCOL *RpmhProtocol = NULL;

  Status = gBS->LocateProtocol(
      &gEfiGpioCrProtocolGuid, NULL, (VOID **)&GpioProtocol);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to locate Gpio CR Protocol: %r\n", Status));
    return Status;
  }

  Status = gBS->LocateProtocol(
      &gEfiRpmhCrProtocolGuid, NULL, (VOID **)&RpmhProtocol);
  if (EFI_ERROR(Status)) {
    DEBUG((DEBUG_ERROR, "Failed to locate Rpmh CR Protocol: %r\n", Status));
    return Status;
  }

  // Enable power rails via RPMH
  // Nothing needed for HDK8450/HDK8350, they use gpio to enable power rails

  // Configure GPIOs for LT9611Uxc
  // Setup Interrupt
  UINT16 GpioIntPin =
      (UINT16)FixedPcdGet32(PcdLt9611GpioInt);
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
  UINT16 *GpioHighArray =
      (UINT16 *)FixedPcdGetPtr(PcdLt9611GpioHigh);
  UINTN GpioHighCount =
      FixedPcdGetSize(PcdLt9611GpioHigh) /
      sizeof(UINT16);

  for (UINTN i = 0; i < GpioHighCount; i++) {
    UINT16 GpioPin = GpioHighArray[i];

    Status = GpioProtocol->SetIoValue(GpioProtocol, GpioPin, GPIO_VALUE_HIGH);
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