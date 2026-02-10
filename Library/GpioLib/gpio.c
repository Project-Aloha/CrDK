/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "gpio_internal.h"
#include <oskal/cr_memory.h>
#ifdef _KERNEL_MODE
#include "gpio.tmh"
#endif

VOID GpioInitConfigParams(OUT GpioConfigParams *ConfigParams,
                          IN UINT16 PinNumber) {
  if (ConfigParams == NULL) {
    log_err(CR_LOG_CHAR8_STR_FMT ": ConfigParams is NULL", __FUNCTION__);
  }
  ConfigParams->PinNumber = PinNumber;
  ConfigParams->OutputEnable = FALSE; // Default to Input
  ConfigParams->FunctionSel = GPIO_FUNC_UNCHANGE;
  ConfigParams->Pull = GPIO_PULL_UNCHANGE;
  ConfigParams->DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE;
  ConfigParams->OutputValue = GPIO_VALUE_UNCHANGE;
}

VOID GpioReadConfig(IN GpioDeviceContext *GpioContext,
                    IN OUT GpioConfigParams *ConfigParams) {
  if (GpioContext == NULL || ConfigParams == NULL ||
      ConfigParams->PinNumber >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return;
  }

  if (GpioContext->GpioPins[ConfigParams->PinNumber].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, ConfigParams->PinNumber);
    return;
  }

  // Read current control register
  UINT32 GpioCtlRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_CTL_REG, ConfigParams->PinNumber));
  UINT32 GpioIoRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_IO_REG, ConfigParams->PinNumber));

  GpioConfigRegInfo *PinRegCfg =
      GpioContext->GpioPins[ConfigParams->PinNumber].pRegCfg;

  ConfigParams->Pull = GET_FIELD(GpioCtlRegVal, PinRegCfg->PullMsk);
  ConfigParams->FunctionSel = GET_FIELD(GpioCtlRegVal, PinRegCfg->MuxSelMsk);
  ConfigParams->DriveStrength =
      GET_FIELD(GpioCtlRegVal, PinRegCfg->DriveStrengthMsk);
  ConfigParams->OutputEnable =
      GET_FIELD(GpioCtlRegVal, PinRegCfg->OutputEnableMsk) ? TRUE : FALSE;

  if (ConfigParams->OutputEnable)
    ConfigParams->OutputValue = GET_FIELD(GpioIoRegVal, PinRegCfg->OutputMsk);
  else
    ConfigParams->OutputValue = GET_FIELD(GpioIoRegVal, PinRegCfg->InputMsk);
}

VOID GpioSetConfig(IN GpioDeviceContext *GpioContext,
                   IN GpioConfigParams *ConfigParams) {
  if (GpioContext == NULL || ConfigParams == NULL ||
      ConfigParams->PinNumber >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return;
  }
  if (GpioContext->GpioPins[ConfigParams->PinNumber].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, ConfigParams->PinNumber);
    return;
  }

  log_info(CR_LOG_CHAR8_STR_FMT
           ": Configuring GPIO %d: FuncSel=%d, OutputEnable=%d, Pull=%d, "
           "DriveStrength=%d, OutputValue=%d",
           __FUNCTION__, ConfigParams->PinNumber, ConfigParams->FunctionSel,
           ConfigParams->OutputEnable, ConfigParams->Pull,
           ConfigParams->DriveStrength, ConfigParams->OutputValue);

  // Read current control register
  UINT32 GpioCtlRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_CTL_REG, ConfigParams->PinNumber));
  UINT32 GpioIoRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_IO_REG, ConfigParams->PinNumber));

  GpioConfigRegInfo *PinRegCfg =
      GpioContext->GpioPins[ConfigParams->PinNumber].pRegCfg;

  // Set mux sel if changed
  if (ConfigParams->FunctionSel != GPIO_FUNC_UNCHANGE) {
    GpioCtlRegVal =
        (CLR_BITS(GpioCtlRegVal, PinRegCfg->MuxSelMsk) |
         SET_FIELD(ConfigParams->FunctionSel, PinRegCfg->MuxSelMsk));
  }

  // Set in/out ctl reg
  GpioCtlRegVal =
      (CLR_BITS(GpioCtlRegVal, PinRegCfg->OutputEnableMsk) |
       SET_FIELD(ConfigParams->OutputEnable, PinRegCfg->OutputEnableMsk));

  // Set output value if output
  if (ConfigParams->OutputEnable) {
    // Set value if output(io reg)
    if (ConfigParams->OutputValue != GPIO_VALUE_UNCHANGE)
      GpioIoRegVal =
          (CLR_BITS(GpioIoRegVal, PinRegCfg->OutputMsk) |
           SET_FIELD(ConfigParams->OutputValue & 1, PinRegCfg->OutputMsk));

    // Clean up pull type for output
    GpioCtlRegVal = (CLR_BITS(GpioCtlRegVal, PinRegCfg->PullMsk) |
                     SET_FIELD(GPIO_PULL_NONE, PinRegCfg->PullMsk));
    // Set drive strength for output
    if (ConfigParams->DriveStrength != GPIO_DRIVE_STRENGTH_UNCHANGE)
      GpioCtlRegVal =
          (CLR_BITS(GpioCtlRegVal, PinRegCfg->DriveStrengthMsk) |
           SET_FIELD(ConfigParams->DriveStrength, PinRegCfg->DriveStrengthMsk));
  } else {
    // Set pull type if input(ctl reg)
    if (ConfigParams->Pull != GPIO_PULL_UNCHANGE) {
      if (GpioContext->PullUpNoKeeper && ConfigParams->Pull == GPIO_PULL_UP) {
        GpioCtlRegVal = (CLR_BITS(GpioCtlRegVal, PinRegCfg->PullMsk) |
                         SET_FIELD(GPIO_PULL_UP_NO_KEEPER, PinRegCfg->PullMsk));
      } else {
        GpioCtlRegVal = (CLR_BITS(GpioCtlRegVal, PinRegCfg->PullMsk) |
                         SET_FIELD(ConfigParams->Pull, PinRegCfg->PullMsk));
      }
    }
  }

  // Write back to registers
  CrMmioWrite32(GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_CTL_REG,
                                     ConfigParams->PinNumber),
                GpioCtlRegVal);
  // Only write io reg when output mode or output value changed
  if (ConfigParams->OutputEnable &&
      ConfigParams->OutputValue != GPIO_VALUE_UNCHANGE)
    CrMmioWrite32(GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_IO_REG,
                                       ConfigParams->PinNumber),
                  GpioIoRegVal);
}

GpioValueType GpioReadInputVal(IN GpioDeviceContext *GpioContext,
                               IN UINT16 GpioIndex) {

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return GPIO_VALUE_LOW;
  }

  // Ignore io mode and only read current io register
  UINT32 GpioIoRegVal = CrMmioRead32(
      GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_IO_REG, GpioIndex));
  return (GET_FIELD(GpioIoRegVal,
                    GpioContext->GpioPins[GpioIndex].pRegCfg->InputMsk))
             ? GPIO_VALUE_HIGH
             : GPIO_VALUE_LOW;
};

GpioValueType GpioReadOutputVal(IN GpioDeviceContext *GpioContext,
                                IN UINT16 GpioIndex) {

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return GPIO_VALUE_LOW;
  }

  // Ignore io mode and only read current io register
  UINT32 GpioIoRegVal = CrMmioRead32(
      GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_IO_REG, GpioIndex));
  return (GET_FIELD(GpioIoRegVal,
                    GpioContext->GpioPins[GpioIndex].pRegCfg->OutputMsk))
             ? GPIO_VALUE_HIGH
             : GPIO_VALUE_LOW;
};

BOOLEAN GpioReadIrqStatus(IN GpioDeviceContext *GpioContext,
                          IN UINT16 GpioIndex) {
  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return FALSE;
  }

  UINT32 IntStatusRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_STATUS_REG, GpioIndex));

  return (IntStatusRegVal &
          GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptStatusMsk)
             ? TRUE
             : FALSE;
}

VOID GpioCleanIrqStatus(IN GpioDeviceContext *GpioContext,
                        IN UINT16 GpioIndex) {

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return;
  }

  // Clear interrupt status
  UINT32 IntStatusRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_STATUS_REG, GpioIndex));
  IntStatusRegVal =
      CLR_BITS(IntStatusRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptStatusMsk);

  // Write Back
  CrMmioWrite32(GpioGetCfgRegByIndex(
                    GpioContext, GPIO_CFG_REG_TYPE_INT_STATUS_REG, GpioIndex),
                IntStatusRegVal);
}

CR_STATUS
GpioCheckInterruptEnabled(IN GpioDeviceContext *GpioContext,
                          IN UINT16 GpioIndex, BOOLEAN *Enabled) {
  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return CR_INVALID_PARAMETER;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    *Enabled = FALSE;
    return CR_SUCCESS;
  }

  UINT32 RegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));

  *Enabled =
      (GET_FIELD(RegVal,
                 GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk) !=
       0);
  return CR_SUCCESS;
}

CR_STATUS
GpioEnableInterrupt(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex,
                    BOOLEAN Enable) {
  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return CR_INVALID_PARAMETER;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return CR_SUCCESS;
  }

  UINT32 RegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));

  if (Enable) {
    RegVal =
        (CLR_BITS(RegVal,
                  GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk) |
         SET_FIELD(1,
                   GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk));
    RegVal =
        (CLR_BITS(
             RegVal,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk) |
         SET_FIELD(
             1,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk));
  } else {
    RegVal = CLR_BITS(RegVal,
                      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk);

    // Read back and check if currenly level triggered
    BOOLEAN IsLevelTrigger =
        GET_FIELD(
            CrMmioRead32(GpioGetCfgRegByIndex(
                GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex)),
            GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionMsk) ==
        0;

    // Only clean raw status for level trigger
    if (IsLevelTrigger)
      RegVal = CLR_BITS(
          RegVal,
          GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk);
  }

  // Write back to register
  CrMmioWrite32(GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG,
                                     GpioIndex),
                RegVal);

  return CR_SUCCESS;
}

CR_STATUS GpioMaskInterrupt(IN GpioDeviceContext *GpioContext,
                            IN UINT16 GpioIndex, IN BOOLEAN Mask) {
  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return CR_INVALID_PARAMETER;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return CR_SUCCESS;
  }

  UINT32 RegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));
  if (Mask) {
    // Read back and check if currenly level triggered
    BOOLEAN IsLevelTrigger =
        GET_FIELD(
            RegVal,
            GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionMsk) ==
        0;
    // Only clean raw status for level trigger
    if (IsLevelTrigger)
      RegVal = CLR_BITS(
          RegVal,
          GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk);
    // Disable interrupt
    RegVal = CLR_BITS(RegVal,
                      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk);
  } else {
    RegVal =
        (CLR_BITS(
             RegVal,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk) |
         SET_FIELD(
             1,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk));
    RegVal =
        (CLR_BITS(RegVal,
                  GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk) |
         SET_FIELD(1,
                   GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk));
  }
  // Write back to register
  CrMmioWrite32(GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG,
                                     GpioIndex),
                RegVal);
  return CR_SUCCESS;
}

CR_STATUS GpioSetInterruptCfg(IN GpioDeviceContext *GpioContext,
                              IN UINT16 GpioIndex,
                              IN CR_INTERRUPT_TRIGGER_TYPE TriggerType) {
  CR_STATUS Status = CR_SUCCESS;
  BOOLEAN IrqEnabled = FALSE;
  UINT32 IntTargetRegVal = 0;
  UINT32 IntCfgRegVal = 0;

  // If width is not 1 or 2, return directly
  if (GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth != 2 &&
      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth != 1) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Unsupported InterruptDetectionWidth %d",
            __FUNCTION__,
            GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth);
    return CR_UNSUPPORTED;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(CR_LOG_CHAR8_STR_FMT
             ": Attempt to operate reserved GPIO %d, ignored",
             __FUNCTION__, GpioIndex);
    return CR_SUCCESS;
  }

  // Route interrupt to apps processor
  IntTargetRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_TARGET_REG, GpioIndex));

  // Use kpss val
  IntTargetRegVal =
      (CLR_BITS(IntTargetRegVal,
                GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptTargetMsk) |
       SET_FIELD(GpioContext->GpioPins[GpioIndex].pRegCfg->TargetKpssVal,
                 GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptTargetMsk));
  CrMmioWrite32(GpioGetCfgRegByIndex(
                    GpioContext, GPIO_CFG_REG_TYPE_INT_TARGET_REG, GpioIndex),
                IntTargetRegVal);

  // Configure interrupt trigger type
  IntCfgRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));
  IrqEnabled =
      GET_FIELD(IntCfgRegVal,
                GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk)
          ? TRUE
          : FALSE;

  // Always set raw status(note: setting raw status will cause an interrupt)
  IntCfgRegVal =
      (CLR_BITS(
           IntCfgRegVal,
           GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk) |
       SET_FIELD(
           1, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk));
  // Clear previous settings
  IntCfgRegVal =
      (CLR_BITS(
           IntCfgRegVal,
           GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionMsk) |
       CLR_BITS(
           IntCfgRegVal,
           GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));

  // Check platform int det width(2 => support dualedge)
  if (GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth == 2) {
    switch ((UINT32)TriggerType) {
    case CR_INTERRUPT_TRIGGER_EDGE_RISING:
      IntCfgRegVal =
          (CLR_BITS(IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                      .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(1, GpioContext->GpioPins[GpioIndex]
                            .pRegCfg->InterruptDetectionMsk));
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      break;
    case CR_INTERRUPT_TRIGGER_EDGE_FALLING:
      IntCfgRegVal =
          (CLR_BITS(IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                      .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(2, GpioContext->GpioPins[GpioIndex]
                            .pRegCfg->InterruptDetectionMsk));
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      break;
    case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
      IntCfgRegVal =
          (CLR_BITS(IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                      .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(3, GpioContext->GpioPins[GpioIndex]
                            .pRegCfg->InterruptDetectionMsk));
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      break;
    case CR_INTERRUPT_TRIGGER_LEVEL_HIGH:
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      break;
    case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
      // Ignore already cleaned bits
      break;
    default:
      log_err(CR_LOG_CHAR8_STR_FMT
              ": Unsupported TriggerType %d, default to Level High",
              __FUNCTION__, TriggerType);
      Status = CR_UNSUPPORTED;
      break;
    }
  } else if (GpioContext->GpioPins[GpioIndex]
                 .pRegCfg->InterruptDetectionWidth == 1) {
    switch ((UINT32)TriggerType) {
    case CR_INTERRUPT_TRIGGER_EDGE_RISING:
      IntCfgRegVal =
          (CLR_BITS(IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                      .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(1, GpioContext->GpioPins[GpioIndex]
                            .pRegCfg->InterruptDetectionMsk));
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      break;
    case CR_INTERRUPT_TRIGGER_EDGE_FALLING:
      IntCfgRegVal =
          (CLR_BITS(IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                      .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(1, GpioContext->GpioPins[GpioIndex]
                            .pRegCfg->InterruptDetectionMsk));
      break;
    case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
      // Need workaround for both edge, ignore currently
      IntCfgRegVal =
          (CLR_BITS(IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                      .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(1, GpioContext->GpioPins[GpioIndex]
                            .pRegCfg->InterruptDetectionMsk));
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      log_warn(
          "Unsupported TriggerType EDGE_BOTH for width 1, default to RISING");
      Status = CR_UNSUPPORTED;
      break;
    case CR_INTERRUPT_TRIGGER_LEVEL_HIGH:
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk) |
           SET_FIELD(
               1,
               GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptPolarityMsk));
      break;
    case CR_INTERRUPT_TRIGGER_LEVEL_LOW:
      // Ignore already cleaned bits
      break;
    default:
      log_err(CR_LOG_CHAR8_STR_FMT
              ": Unsupported TriggerType %d, default to Level High",
              __FUNCTION__, TriggerType);
      Status = CR_UNSUPPORTED;
      break;
    }
  }

  // Setting raw status will cause an interrupt, clean it
  if (!IrqEnabled)
    GpioCleanIrqStatus(GpioContext, GpioIndex);

  // Write back to register
  CrMmioWrite32(GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG,
                                     GpioIndex),
                IntCfgRegVal);

  return Status;
}

CR_STATUS GpioLibInit(IN OUT GpioDeviceContext **GpioContext) {
  if (GpioContext == NULL)
    return CR_INVALID_PARAMETER;

  // Get Target Info if not provided
  if (*GpioContext == NULL) {
    *GpioContext = CrTargetGetGpioContext();
    if (*GpioContext == NULL) {
      log_err("CrTargetGetGpioContext failed.");
      return CR_NOT_FOUND;
    }
  }

  // // Init Gpio Interrupt
  // Status = GpioInitIrq(*GpioContext);
  // if (CR_ERROR(Status)) {
  //   log_err("GpioInitIrq failed: 0x%08X", Status);
  //   return Status;
  // }
  return CR_SUCCESS;
}