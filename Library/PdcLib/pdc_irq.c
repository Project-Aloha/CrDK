#include "pdc_internal.h"

CR_STATUS PdcRegisterInterrupt(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                               CR_INTERRUPT_HANDLER InterruptHandler,
                               VOID *Param, CR_INTERRUPT_TRIGGER_TYPE Type) {
  CR_STATUS Status = CR_SUCCESS;
  UINT32 GicIrq = PdcGetGicIrqFromPdcPin(Ctx, PdcPinNumber);

  // Set up Pdc and get gic type
  CR_INTERRUPT_TRIGGER_TYPE GicType;
  Status = PdcGicSetType(Ctx, PdcPinNumber, Type, &GicType);
  if (CR_ERROR(Status)) {
    log_err(CR_LOG_CHAR8_STR_FMT
            ": Failed to set PDC GIC type for PDC Pin %d, Status=0x%X",
            __FUNCTION__, PdcPinNumber, Status);
    return Status;
  }

  // Enable Pdc Interrupt
  PdcEnableInterrupt(Ctx, PdcPinNumber, TRUE);

  // Register directly to interrupt controller
  log_info(CR_LOG_CHAR8_STR_FMT
           ": Registering interrupt for PDC Pin %d (GIC IRQ %d) with type %d",
           __FUNCTION__, PdcPinNumber, GicIrq, GicType);
  Status = CrRegisterInterrupt(GicIrq, InterruptHandler, Param, GicType);
  if (CR_ERROR(Status)) {
    log_err(CR_LOG_CHAR8_STR_FMT
            ": Failed to register interrupt for PDC Pin %d (GIC IRQ %d), "
            "Status=0x%X",
            __FUNCTION__, PdcPinNumber, GicIrq, Status);
    return Status;
  }

  return Status;
}

CR_STATUS
PdcUnregisterInterrupt(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                       CR_INTERRUPT_TRIGGER_TYPE Type) {
  CR_STATUS Status = CR_SUCCESS;
  UINT32 GicIrq = PdcGetGicIrqFromPdcPin(Ctx, PdcPinNumber);
  // Disable Pdc Interrupt
  PdcEnableInterrupt(Ctx, PdcPinNumber, FALSE);

  // Unregister directly from interrupt control ler
  Status = CrUnregisterInterrupt(GicIrq);
  if (CR_ERROR(Status)) {
    log_err(CR_LOG_CHAR8_STR_FMT
            ": Failed to unregister interrupt for PDC Pin %d (GIC IRQ %d), "
            "Status=0x%X",
            __FUNCTION__, PdcPinNumber, GicIrq, Status);
    return Status;
  }
  return Status;
}
