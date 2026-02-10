#include "gpio_internal.h"
#include <Library/pdc.h>
typedef struct {
  CR_INTERRUPT_HANDLER Handler;
  VOID                *Param;
  GpioPinInfo         *PinInfo;
} GPIO_INTERRUPT_ENTRY;

// Internal GPIO IRQ table
STATIC GPIO_INTERRUPT_ENTRY mGpioIntTable[GPIO_PINS_MAX];

// ISR on GPIO interrupt
VOID GpioIsr(VOID *context_param)
{
  // Check irq enabled pins's irq status register
  GpioDeviceContext   *Context = (GpioDeviceContext *)context_param;
  GpioPinInfo         *PinInfo = NULL;
  CR_INTERRUPT_HANDLER Handler = NULL;
  UINT16               Handled = 0;

  log_info("GpioIsr: Handling GPIO interrupts");

  for (UINT16 i = 0; i < GPIO_PINS_MAX; i++) {
    PinInfo = mGpioIntTable[i].PinInfo;
    Handler = mGpioIntTable[i].Handler;

    if ((PinInfo != NULL) && (PinInfo->PinNumber < Context->PinCount) &&
        (Handler != NULL)) {
      // Read interrupt status register
      UINT32 IntStatusRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
          Context, GPIO_CFG_REG_TYPE_INT_STATUS_REG, PinInfo->PinNumber));
      GpioInterruptStatusRegInfo *IntStatusRegInfo = (VOID *)&IntStatusRegVal;
      if (IntStatusRegInfo->InterruptStatus) {
        // Call registered handler with cust param
        Handler(mGpioIntTable[i].Param);
        // Ignore Interrupt Ack high flag for new platforms.
        GpioCleanIrqStatus(Context, PinInfo->PinNumber);
        log_info("GpioIsr: Handled IRQ on Gpio %d", PinInfo->PinNumber);
      }
      Handled++;
    }
    else {
      // Reached empty entry, stop checking
      if (Handled == 0) {
        log_err(
            CR_LOG_CHAR8_STR_FMT ": Spurious IRQ, no handler found for any GPIO pin",
            __FUNCTION__);
      }
      break;
    }
  }
}

// Register IRQ on a GPIO pin
CR_STATUS
GpioRegisterGpioIrq(
    IN GpioDeviceContext *Context, IN UINT16 GpioIndex,
    IN CR_INTERRUPT_HANDLER InterruptHandler, IN VOID *Param,
    IN CR_INTERRUPT_TRIGGER_TYPE TriggerType)
{
  CR_STATUS Status = CR_SUCCESS;

  if (GpioIndex >= GPIO_PINS_MAX) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid GpioIndex %d", __FUNCTION__, GpioIndex);
    return CR_INVALID_PARAMETER;
  }

  // If this pin is not a PDC pin, use shared IRQ
  if (Context->GpioPins[GpioIndex].PdcPinNumber ==
      GPIO_PDC_PIN_NUMBER_INVALID) {
    log_info(
        CR_LOG_CHAR8_STR_FMT ": Gpio %d is not a PDC pin, registering interrupt via shared IRQ",
        __FUNCTION__, GpioIndex);

    // Find a free entry and register
    UINTN i;
    for (i = 0; i < GPIO_PINS_MAX; i++) {
      if (mGpioIntTable[i].Handler == NULL) {
        mGpioIntTable[i].Handler = InterruptHandler;
        mGpioIntTable[i].Param   = Param;
        mGpioIntTable[i].PinInfo = &Context->GpioPins[GpioIndex];
        break;
      }
    }
    if (i == GPIO_PINS_MAX) {
      log_err(CR_LOG_CHAR8_STR_FMT ": No free interrupt entries available", __FUNCTION__);
      return CR_OUT_OF_RESOURCES;
    }

    // Configure interrupt
    Status = GpioSetInterruptCfg(Context, GpioIndex, TriggerType);
    if (CR_ERROR(Status)) {
      log_err(
          CR_LOG_CHAR8_STR_FMT ": Failed to configure interrupt for Gpio %d, Status=0x%X",
          __FUNCTION__, GpioIndex, Status);
      return Status;
    }

    // Enable interrupt
    Status = GpioEnableInterrupt(Context, GpioIndex, TRUE, TriggerType);
    if (CR_ERROR(Status)) {
      log_err(
          CR_LOG_CHAR8_STR_FMT ": Failed to enable interrupt for Gpio %d, Status=0x%X",
          __FUNCTION__, GpioIndex, Status);
    }
  }
  else {
    // If it is a PDC pin, it has its own GIC SPI IRQ.
    // Call PdcLib to register directly.
    log_info(
        CR_LOG_CHAR8_STR_FMT ": Gpio %d is a PDC pin, registering interrupt via PdcLib",
        __FUNCTION__, GpioIndex);
    Status = PdcRegisterInterrupt(
        Context->GpioPins[GpioIndex].PdcPinNumber, // PDC Port Number
        InterruptHandler,                          // Handler
        Param,                                     // Parameters
        TriggerType);                              // Gpio Trigger Type
  }
  return Status;
}

// Unregister IRQ on a GPIO pin
CR_STATUS
GpioUnregisterGpioIrq(
    IN GpioDeviceContext *Context, IN UINT16 GpioIndex,
    IN CR_INTERRUPT_TRIGGER_TYPE TriggerType)
{
  CR_STATUS Status = CR_SUCCESS;

  if (GpioIndex >= GPIO_PINS_MAX) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid GpioIndex %d", __FUNCTION__, GpioIndex);
    return CR_INVALID_PARAMETER;
  }

  // Disable interrupt
  Status = GpioEnableInterrupt(Context, GpioIndex, FALSE, TriggerType);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": Failed to enable interrupt for Gpio %d, Status=0x%X", __FUNCTION__,
        GpioIndex, Status);
  }

  // Find the entry and move entries forward
  {
    UINTN i;
    for (i = 0; i < GPIO_PINS_MAX; i++) {
      if (mGpioIntTable[i].PinInfo != NULL &&
          mGpioIntTable[i].PinInfo->PinNumber == GpioIndex) {
        // Clear the entry
        mGpioIntTable[i].Handler = NULL;
        mGpioIntTable[i].Param   = NULL;
        mGpioIntTable[i].PinInfo = NULL;
        break;
      }
    }
    if (i == GPIO_PINS_MAX) {
      log_err(
          CR_LOG_CHAR8_STR_FMT ": GpioIndex %d not found in interrupt table", __FUNCTION__,
          GpioIndex);
      return CR_NOT_FOUND;
    }

    // Move other entries forward to fill the gap
    for (; i < GPIO_PINS_MAX - 1; i++) {
      if (mGpioIntTable[i + 1].Handler != NULL) {
        mGpioIntTable[i] = mGpioIntTable[i + 1];
        // Clear the moved entry
        mGpioIntTable[i + 1].Handler = NULL;
        mGpioIntTable[i + 1].Param   = NULL;
        mGpioIntTable[i + 1].PinInfo = NULL;
      }
      else {
        break;
      }
    }
  }

  return CR_SUCCESS;
}

// Init IRQ
CR_STATUS
GpioInitIrq(GpioDeviceContext *Context)
{
  CR_STATUS Status;

  // Register interrupt handler
  Status = CrRegisterInterrupt(
      Context->InterruptNo,             // Irq Number
      GpioIsr,                          // Handler
      Context,                          // Param
      CR_INTERRUPT_TRIGGER_LEVEL_HIGH); // Level trigger

  if (CR_ERROR(Status)) {
    log_err("Failed to register interrupt, Status=0x%X", Status);
    return Status;
  }
  log_info("GpioInitIrq: Registered IRQ %d for GPIO", Context->InterruptNo);
  return CR_SUCCESS;
}
