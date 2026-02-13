#include "pdc_internal.h"

#ifndef _KERNEL_MODE

CR_STATUS PdcRegisterInterrupt(
    PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
    CR_INTERRUPT_HANDLER InterruptHandler, VOID *Param,
    CR_INTERRUPT_TRIGGER_TYPE Type)
{
  CR_STATUS Status = CR_SUCCESS;
  UINT16    GicIrq = 0;
  Status           = PdcGetGicIrqFromPdcPin(Ctx, PdcPinNumber, &GicIrq);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT
        ": Failed to get GIC IRQ from PDC Pin %d, Status=0x%X",
        __FUNCTION__, PdcPinNumber, Status);
    return Status;
  }

  // Set up Pdc and get gic type
  CR_INTERRUPT_TRIGGER_TYPE GicType;
  Status = PdcGicSetType(Ctx, PdcPinNumber, Type, &GicType);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT
        ": Failed to set PDC GIC type for PDC Pin %d, Status=0x%X",
        __FUNCTION__, PdcPinNumber, Status);
    return Status;
  }

  // Enable Pdc Interrupt
  PdcEnableInterrupt(Ctx, PdcPinNumber, TRUE);

  // Register directly to interrupt controller
  log_info(
      CR_LOG_CHAR8_STR_FMT
      ": Registering interrupt for PDC Pin %d (GIC IRQ %d) with type %d",
      __FUNCTION__, PdcPinNumber, GicIrq, GicType);

  CR_INTERRUPT_CONFIG InterruptConfig = {
      .InterruptNumber = GicIrq,
      .Handler         = InterruptHandler,
      .Param           = Param,
      .TriggerType     = GicType,
  };
  Status = CrRegisterInterrupt(&InterruptConfig);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT
        ": Failed to register interrupt for PDC Pin %d (GIC IRQ %d), "
        "Status=0x%X",
        __FUNCTION__, PdcPinNumber, GicIrq, Status);
    return Status;
  }

  return Status;
}

CR_STATUS
PdcUnregisterInterrupt(
    PdcDeviceContext *Ctx, UINT16 PdcPinNumber, CR_INTERRUPT_TRIGGER_TYPE Type)
{
  CR_STATUS Status = CR_SUCCESS;
  UINT16    GicIrq = 0;
  Status           = PdcGetGicIrqFromPdcPin(Ctx, PdcPinNumber, &GicIrq);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT
        ": Failed to get GIC IRQ from PDC Pin %d, Status=0x%X",
        __FUNCTION__, PdcPinNumber, Status);
    return Status;
  }

  // Disable Pdc Interrupt
  PdcEnableInterrupt(Ctx, PdcPinNumber, FALSE);

  // Unregister directly from interrupt control ler
  CR_INTERRUPT_CONFIG InterruptConfig = {
      .InterruptNumber = GicIrq,
  };
  Status = CrUnregisterInterrupt(&InterruptConfig);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT
        ": Failed to unregister interrupt for PDC Pin %d (GIC IRQ %d), "
        "Status=0x%X",
        __FUNCTION__, PdcPinNumber, GicIrq, Status);
    return Status;
  }
  return Status;
}

#endif