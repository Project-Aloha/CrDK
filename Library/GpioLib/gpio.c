/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "gpio_internal.h"
#include <oskal/cr_memory.h>
#ifdef _KERNEL_MODE
#if !defined(CR_OSK_LOG_WITHOUT_WPP)
#include "gpio.tmh"
#endif
#endif

STATIC BOOLEAN GpioPinValid(GpioDeviceContext *Context, UINT16 Pin) {
  return Context != NULL && Context->PinCount <= GPIO_PINS_MAX &&
    Pin < Context->PinCount && Context->GpioPins[Pin].pRegCfg != NULL;
}

VOID GpioInitConfigParams(
    OUT GpioConfigParams *ConfigParams, IN UINT16 PinNumber)
{
  if (ConfigParams == NULL) {
    log_err(CR_LOG_CHAR8_STR_FMT ": ConfigParams is NULL", __FUNCTION__);
    return;
  }
  ConfigParams->PinNumber     = PinNumber;
  ConfigParams->OutputEnable  = FALSE; // Default to Input
  ConfigParams->FunctionSel   = GPIO_FUNC_UNCHANGE;
  ConfigParams->Pull          = GPIO_PULL_UNCHANGE;
  ConfigParams->DriveStrength = GPIO_DRIVE_STRENGTH_UNCHANGE;
  ConfigParams->OutputValue   = GPIO_VALUE_UNCHANGE;
}

VOID GpioReadConfig(
    IN GpioDeviceContext *GpioContext, IN OUT GpioConfigParams *ConfigParams)
{
  if (ConfigParams == NULL || !GpioPinValid(GpioContext, ConfigParams->PinNumber)) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return;
  }

  if (GpioContext->GpioPins[ConfigParams->PinNumber].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, ConfigParams->PinNumber);
    return;
  }

  // Read current control register
  UINT32 GpioCtlRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_CTL_REG, ConfigParams->PinNumber));
  GpioConfigRegInfo *PinRegCfg =
      GpioContext->GpioPins[ConfigParams->PinNumber].pRegCfg;
  UINT32 GpioIoRegVal = 0;
  if (PinRegCfg->InputMsk != 0 || PinRegCfg->OutputMsk != 0)
    GpioIoRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
        GpioContext, GPIO_CFG_REG_TYPE_IO_REG, ConfigParams->PinNumber));

  ConfigParams->Pull = PinRegCfg->PullMsk != 0 ?
      GET_FIELD(GpioCtlRegVal, PinRegCfg->PullMsk) : GPIO_PULL_UNCHANGE;
  ConfigParams->FunctionSel = PinRegCfg->MuxSelMsk != 0 ?
      GET_FIELD(GpioCtlRegVal, PinRegCfg->MuxSelMsk) : GPIO_FUNC_UNCHANGE;
  if (GpioContext->EgpioFunction != GPIO_FUNC_NORMAL &&
      (GpioCtlRegVal & PinRegCfg->EgpioPresentMsk) != 0 &&
      PinRegCfg->EgpioEnabledMsk != 0 &&
      (GpioCtlRegVal & PinRegCfg->EgpioEnabledMsk) == 0)
    ConfigParams->FunctionSel = GpioContext->EgpioFunction;
  ConfigParams->DriveStrength = PinRegCfg->DriveStrengthMsk != 0 ?
      GET_FIELD(GpioCtlRegVal, PinRegCfg->DriveStrengthMsk) :
      GPIO_DRIVE_STRENGTH_UNCHANGE;
  /* Linux marks UFS_RESET's OE/input bits absent: it is output-only.
     SDC pads have neither input nor output registers. Never shift a zero
     field mask or read a register that the target does not implement. */
  ConfigParams->OutputEnable = PinRegCfg->OutputEnableMsk != 0 ?
      (GET_FIELD(GpioCtlRegVal, PinRegCfg->OutputEnableMsk) != 0) :
      (PinRegCfg->OutputMsk != 0);

  if (ConfigParams->OutputEnable && PinRegCfg->OutputMsk != 0)
    ConfigParams->OutputValue = GET_FIELD(GpioIoRegVal, PinRegCfg->OutputMsk);
  else if (PinRegCfg->InputMsk != 0)
    ConfigParams->OutputValue = GET_FIELD(GpioIoRegVal, PinRegCfg->InputMsk);
  else
    ConfigParams->OutputValue = GPIO_VALUE_UNCHANGE;
}

VOID GpioSetConfig(
    IN GpioDeviceContext *GpioContext, IN GpioConfigParams *ConfigParams)
{
  if (ConfigParams == NULL || !GpioPinValid(GpioContext, ConfigParams->PinNumber)) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return;
  }
  if (GpioContext->GpioPins[ConfigParams->PinNumber].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, ConfigParams->PinNumber);
    return;
  }

  log_info(
      CR_LOG_CHAR8_STR_FMT
      ": Configuring GPIO %d: FuncSel=%d, OutputEnable=%d, Pull=%d, "
      "DriveStrength=%d, OutputValue=%d",
      __FUNCTION__, ConfigParams->PinNumber, ConfigParams->FunctionSel,
      ConfigParams->OutputEnable, ConfigParams->Pull,
      ConfigParams->DriveStrength, ConfigParams->OutputValue);

  // Read current control register
  UINT32 GpioCtlRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_CTL_REG, ConfigParams->PinNumber));
  GpioConfigRegInfo *PinRegCfg =
      GpioContext->GpioPins[ConfigParams->PinNumber].pRegCfg;
  UINT32 GpioIoRegVal = 0;
  BOOLEAN WriteOutput = ConfigParams->OutputEnable &&
      ConfigParams->OutputValue != GPIO_VALUE_UNCHANGE &&
      PinRegCfg->OutputMsk != 0;
  if (WriteOutput)
    GpioIoRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
        GpioContext, GPIO_CFG_REG_TYPE_IO_REG, ConfigParams->PinNumber));

  // Set mux sel if changed
  if (ConfigParams->FunctionSel != GPIO_FUNC_UNCHANGE &&
      PinRegCfg->MuxSelMsk != 0) {
    /* Linux msm_pinmux_set_mux(): EGPIO ownership sits outside the mux
       field. Selecting a local function must reclaim the physical pad. */
    if (GpioContext->EgpioFunction != GPIO_FUNC_NORMAL &&
        ConfigParams->FunctionSel == GpioContext->EgpioFunction) {
      if ((GpioCtlRegVal & PinRegCfg->EgpioPresentMsk) != 0)
        GpioCtlRegVal &= ~PinRegCfg->EgpioEnabledMsk;
    } else {
      GpioCtlRegVal =
          (CLR_BITS(GpioCtlRegVal, PinRegCfg->MuxSelMsk) |
           SET_FIELD(ConfigParams->FunctionSel, PinRegCfg->MuxSelMsk));
      if (GpioContext->EgpioFunction != GPIO_FUNC_NORMAL &&
          (GpioCtlRegVal & PinRegCfg->EgpioPresentMsk) != 0) {
        if ((GpioCtlRegVal & PinRegCfg->EgpioEnabledMsk) == 0)
          log_info("GPIO %d EGPIO AP ownership requested ctl=0x%08x",
                   ConfigParams->PinNumber, GpioCtlRegVal | PinRegCfg->EgpioEnabledMsk);
        GpioCtlRegVal |= PinRegCfg->EgpioEnabledMsk;
      }
    }
  }

  // Set in/out ctl reg
  if (PinRegCfg->OutputEnableMsk != 0)
    GpioCtlRegVal =
        (CLR_BITS(GpioCtlRegVal, PinRegCfg->OutputEnableMsk) |
         SET_FIELD(ConfigParams->OutputEnable, PinRegCfg->OutputEnableMsk));

  // Pull and drive strength are pad properties, independent of direction.
  if (ConfigParams->Pull != GPIO_PULL_UNCHANGE && PinRegCfg->PullMsk != 0) {
    GpioPullType Pull = ConfigParams->Pull;
    if (GpioContext->PullUpNoKeeper && Pull == GPIO_PULL_UP)
      Pull = GPIO_PULL_UP_NO_KEEPER;
    GpioCtlRegVal =
        (CLR_BITS(GpioCtlRegVal, PinRegCfg->PullMsk) |
         SET_FIELD(Pull, PinRegCfg->PullMsk));
  }
  if (ConfigParams->DriveStrength != GPIO_DRIVE_STRENGTH_UNCHANGE &&
      PinRegCfg->DriveStrengthMsk != 0)
    GpioCtlRegVal =
        (CLR_BITS(GpioCtlRegVal, PinRegCfg->DriveStrengthMsk) |
         SET_FIELD(ConfigParams->DriveStrength, PinRegCfg->DriveStrengthMsk));

  // Preload the output latch before enabling the driver or changing its mux.
  if (WriteOutput) {
    GpioIoRegVal =
        (CLR_BITS(GpioIoRegVal, PinRegCfg->OutputMsk) |
         SET_FIELD(ConfigParams->OutputValue & 1, PinRegCfg->OutputMsk));
    CrMmioWrite32(
        GpioGetCfgRegByIndex(
            GpioContext, GPIO_CFG_REG_TYPE_IO_REG, ConfigParams->PinNumber),
        GpioIoRegVal);
  }
  CrMmioWrite32(
      GpioGetCfgRegByIndex(
          GpioContext, GPIO_CFG_REG_TYPE_CTL_REG, ConfigParams->PinNumber),
      GpioCtlRegVal);
}

GpioValueType
GpioReadInputVal(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex)
{
  if (!GpioPinValid(GpioContext, GpioIndex) ||
      GpioContext->GpioPins[GpioIndex].pRegCfg->InputMsk == 0)
    return GPIO_VALUE_LOW;

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, GpioIndex);
    return GPIO_VALUE_LOW;
  }

  // Ignore io mode and only read current io register
  UINT32 GpioIoRegVal = CrMmioRead32(
      GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_IO_REG, GpioIndex));
  return (GET_FIELD(
             GpioIoRegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->InputMsk))
             ? GPIO_VALUE_HIGH
             : GPIO_VALUE_LOW;
};

GpioValueType
GpioReadOutputVal(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex)
{
  if (!GpioPinValid(GpioContext, GpioIndex) ||
      GpioContext->GpioPins[GpioIndex].pRegCfg->OutputMsk == 0)
    return GPIO_VALUE_LOW;

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, GpioIndex);
    return GPIO_VALUE_LOW;
  }

  // Ignore io mode and only read current io register
  UINT32 GpioIoRegVal = CrMmioRead32(
      GpioGetCfgRegByIndex(GpioContext, GPIO_CFG_REG_TYPE_IO_REG, GpioIndex));
  return (GET_FIELD(
             GpioIoRegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->OutputMsk))
             ? GPIO_VALUE_HIGH
             : GPIO_VALUE_LOW;
};

CR_STATUS GpioApplyConfigChecked(GpioDeviceContext *Context,CONST GpioConfigParams *Config) {
  if(Config==NULL||!GpioPinValid(Context,Config->PinNumber)||Context->TlmmBaseAddress==0||Config->OutputEnable>1)
    return CR_INVALID_PARAMETER;
  GpioPinInfo *Pin=&Context->GpioPins[Config->PinNumber];GpioConfigRegInfo *Reg=Pin->pRegCfg;
  if(Pin->Reserved)return CR_BUSY;
  if(Reg->OutputEnableMsk==0 || Reg->MuxSelMsk==0 || Reg->PullMsk==0 || Reg->DriveStrengthMsk==0 || Reg->OutputMsk==0)
    return CR_UNSUPPORTED;
  if(Config->FunctionSel>GPIO_FUNC_MAX||Config->Pull>GPIO_PULL_MAX||Config->DriveStrength>GPIO_DRIVE_STRENGTH_MAX||Config->OutputValue>GPIO_VALUE_MAX ||
     !(Pin->FuncMuxMap&(1U<<Config->FunctionSel)))return CR_INVALID_PARAMETER;
  /* Restore the original latent latch even when OE remains off. Enabling the
   * output temporarily to achieve this would create an unwanted pad pulse. */
  if(!Config->OutputEnable) {
    UINTN Io=GpioGetCfgRegByIndex(Context,GPIO_CFG_REG_TYPE_IO_REG,Config->PinNumber);
    UINT32 Value=CrMmioRead32(Io);Value=(Value&~Reg->OutputMsk)|SET_FIELD(Config->OutputValue,Reg->OutputMsk);
    CrMmioWrite32(Io,Value);
  }
  GpioConfigParams Desired=*Config;GpioSetConfig(Context,&Desired);
  GpioConfigParams Actual;GpioInitConfigParams(&Actual,Config->PinNumber);GpioReadConfig(Context,&Actual);
  Actual.OutputValue=GpioReadOutputVal(Context,Config->PinNumber);
  GpioPullType Pull=Context->PullUpNoKeeper&&Config->Pull==GPIO_PULL_UP ? GPIO_PULL_UP_NO_KEEPER : Config->Pull;
  if(Actual.OutputEnable!=Config->OutputEnable||Actual.FunctionSel!=Config->FunctionSel||Actual.Pull!=Pull||
      Actual.DriveStrength!=Config->DriveStrength||Actual.OutputValue!=Config->OutputValue)return CR_DEVICE_ERROR;
  return CR_SUCCESS;
}

STATIC BOOLEAN GpioHasInterrupt(GpioDeviceContext *Context, UINT16 Pin) {
  return GpioPinValid(Context, Pin) &&
    Context->GpioPins[Pin].pRegCfg->InterruptEnMsk != 0 &&
    Context->GpioPins[Pin].pRegCfg->InterruptDetectionMsk != 0 &&
    Context->GpioPins[Pin].pRegCfg->InterruptRawStatusMsk != 0;
}

BOOLEAN
GpioReadIrqStatus(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex)
{
  if (!GpioPinValid(GpioContext, GpioIndex) ||
      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptStatusMsk == 0) return FALSE;

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
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

VOID GpioCleanIrqStatus(IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex)
{
  if (!GpioPinValid(GpioContext, GpioIndex) ||
      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptStatusMsk == 0) return;


  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, GpioIndex);
    return;
  }

  // Clear interrupt status
  UINT32 IntStatusRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_STATUS_REG, GpioIndex));
  IntStatusRegVal        = CLR_BITS(
      IntStatusRegVal,
      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptStatusMsk);

  // Write Back
  CrMmioWrite32(
      GpioGetCfgRegByIndex(
          GpioContext, GPIO_CFG_REG_TYPE_INT_STATUS_REG, GpioIndex),
      IntStatusRegVal);
}

CR_STATUS
GpioCheckInterruptEnabled(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex, BOOLEAN *Enabled)
{
  if (Enabled == NULL || !GpioPinValid(GpioContext, GpioIndex)) return CR_INVALID_PARAMETER;
  *Enabled = FALSE;
  if (!GpioHasInterrupt(GpioContext, GpioIndex)) return CR_UNSUPPORTED;

  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return CR_INVALID_PARAMETER;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, GpioIndex);
    *Enabled = FALSE;
    return CR_SUCCESS;
  }

  UINT32 RegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));

  *Enabled =
      (GET_FIELD(
           RegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk) !=
       0);
  return CR_SUCCESS;
}

CR_STATUS
GpioEnableInterrupt(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex, BOOLEAN Enable)
{
  if (Enable > TRUE) return CR_INVALID_PARAMETER;
  if (!GpioPinValid(GpioContext, GpioIndex)) return CR_INVALID_PARAMETER;
  if (!GpioHasInterrupt(GpioContext, GpioIndex)) return CR_UNSUPPORTED;

  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return CR_INVALID_PARAMETER;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, GpioIndex);
    return CR_SUCCESS;
  }

  UINT32 RegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));

  if (Enable) {
    RegVal =
        (CLR_BITS(
             RegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk) |
         SET_FIELD(
             1, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk));
    RegVal =
        (CLR_BITS(
             RegVal,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk) |
         SET_FIELD(
             1,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk));
  }
  else {
    RegVal = CLR_BITS(
        RegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk);

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
  CrMmioWrite32(
      GpioGetCfgRegByIndex(
          GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex),
      RegVal);

  return CR_SUCCESS;
}

CR_STATUS GpioMaskInterrupt(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex, IN BOOLEAN Mask)
{
  if (Mask > TRUE) return CR_INVALID_PARAMETER;
  if (!GpioPinValid(GpioContext, GpioIndex)) return CR_INVALID_PARAMETER;
  if (!GpioHasInterrupt(GpioContext, GpioIndex)) return CR_UNSUPPORTED;

  if (GpioContext == NULL || GpioIndex >= GpioContext->PinCount) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid parameters", __FUNCTION__);
    return CR_INVALID_PARAMETER;
  }

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
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
    RegVal = CLR_BITS(
        RegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk);
  }
  else {
    RegVal =
        (CLR_BITS(
             RegVal,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk) |
         SET_FIELD(
             1,
             GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptRawStatusMsk));
    RegVal =
        (CLR_BITS(
             RegVal, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk) |
         SET_FIELD(
             1, GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptEnMsk));
  }
  // Write back to register
  CrMmioWrite32(
      GpioGetCfgRegByIndex(
          GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex),
      RegVal);
  return CR_SUCCESS;
}

CR_STATUS GpioSetInterruptCfg(
    IN GpioDeviceContext *GpioContext, IN UINT16 GpioIndex,
    IN CR_INTERRUPT_TRIGGER_TYPE TriggerType)
{
  if (!GpioPinValid(GpioContext, GpioIndex)) return CR_INVALID_PARAMETER;
  if (!GpioHasInterrupt(GpioContext, GpioIndex)) return CR_UNSUPPORTED;

  CR_STATUS Status          = CR_SUCCESS;
  BOOLEAN   IrqEnabled      = FALSE;
  UINT32    IntTargetRegVal = 0;
  UINT32    IntCfgRegVal    = 0;

  // If width is not 1 or 2, return directly
  if (GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth != 2 &&
      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth != 1) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": Unsupported InterruptDetectionWidth %d",
        __FUNCTION__,
        GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth);
    return CR_UNSUPPORTED;
  }
  if ((UINT32)TriggerType > CR_INTERRUPT_TRIGGER_EDGE_BOTH)
    return CR_INVALID_PARAMETER;
  if (GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth == 1 &&
      TriggerType == CR_INTERRUPT_TRIGGER_EDGE_BOTH)
    return CR_UNSUPPORTED;

  if (GpioContext->GpioPins[GpioIndex].Reserved) {
    log_warn(
        CR_LOG_CHAR8_STR_FMT ": Attempt to operate reserved GPIO %d, ignored",
        __FUNCTION__, GpioIndex);
    return CR_SUCCESS;
  }

  // Route interrupt to apps processor
  IntTargetRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_TARGET_REG, GpioIndex));

  // Use kpss val
  IntTargetRegVal =
      (CLR_BITS(
           IntTargetRegVal,
           GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptTargetMsk) |
       SET_FIELD(
           GpioContext->GpioPins[GpioIndex].pRegCfg->TargetKpssVal,
           GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptTargetMsk));
  CrMmioWrite32(
      GpioGetCfgRegByIndex(
          GpioContext, GPIO_CFG_REG_TYPE_INT_TARGET_REG, GpioIndex),
      IntTargetRegVal);

  // Configure interrupt trigger type
  IntCfgRegVal = CrMmioRead32(GpioGetCfgRegByIndex(
      GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex));
  IrqEnabled =
      GET_FIELD(
          IntCfgRegVal,
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
          (CLR_BITS(
               IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                 .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(
               1, GpioContext->GpioPins[GpioIndex]
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
          (CLR_BITS(
               IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                 .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(
               2, GpioContext->GpioPins[GpioIndex]
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
          (CLR_BITS(
               IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                 .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(
               3, GpioContext->GpioPins[GpioIndex]
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
      log_err(
          CR_LOG_CHAR8_STR_FMT
          ": Unsupported TriggerType %d, default to Level High",
          __FUNCTION__, TriggerType);
      Status = CR_UNSUPPORTED;
      break;
    }
  }
  else if (
      GpioContext->GpioPins[GpioIndex].pRegCfg->InterruptDetectionWidth == 1) {
    switch ((UINT32)TriggerType) {
    case CR_INTERRUPT_TRIGGER_EDGE_RISING:
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                 .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(
               1, GpioContext->GpioPins[GpioIndex]
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
          (CLR_BITS(
               IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                 .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(
               1, GpioContext->GpioPins[GpioIndex]
                      .pRegCfg->InterruptDetectionMsk));
      break;
    case CR_INTERRUPT_TRIGGER_EDGE_BOTH:
      // Need workaround for both edge, ignore currently
      IntCfgRegVal =
          (CLR_BITS(
               IntCfgRegVal, GpioContext->GpioPins[GpioIndex]
                                 .pRegCfg->InterruptDetectionMsk) |
           SET_FIELD(
               1, GpioContext->GpioPins[GpioIndex]
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
      log_err(
          CR_LOG_CHAR8_STR_FMT
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
  CrMmioWrite32(
      GpioGetCfgRegByIndex(
          GpioContext, GPIO_CFG_REG_TYPE_INT_CFG_REG, GpioIndex),
      IntCfgRegVal);

  return Status;
}

CR_STATUS GpioLibInit(IN OUT GpioDeviceContext **GpioContext)
{
  if (GpioContext == NULL)
    return CR_INVALID_PARAMETER;

  if (*GpioContext == NULL) {
    log_err("GpioLibInit requires a target context.");
    return CR_NOT_FOUND;
  }
  if ((*GpioContext)->ActiveInterrupts != 0) return CR_BUSY;

  if ((*GpioContext)->TlmmBaseAddress == 0 || (*GpioContext)->PinCount == 0 ||
      (*GpioContext)->PinCount > GPIO_PINS_MAX) return CR_INVALID_PARAMETER;
  for (UINT16 I = 0; I < (*GpioContext)->PinCount; ++I) {
    GpioPinInfo *Pin = &(*GpioContext)->GpioPins[I];
    if (Pin->pTileInfo == NULL || Pin->pRegCfg == NULL || Pin->pRegCfg->Index >= GPIO_MAX_REG_CFG)
      return CR_INVALID_PARAMETER;
  }

  // Calculate each pins base addresses
  for (UINT16 i = 0; i < (*GpioContext)->PinCount; i++) {
    if ((*GpioContext)->GpioPins[i].pRegCfg->Index == 0) {
      (*GpioContext)->GpioPins[i].PinAddress =
          (*GpioContext)->TlmmBaseAddress +
          (*GpioContext)->GpioPins[i].pTileInfo->TileOffset +
          (i * GPIO_TLMM_PIN_STRIDE);
    }
    else {
      /* Special-pad register offsets are relative to TLMM, not to a
         per-pin bank. GpioGetCfgRegByIndex applies that offset once.
         SM8450 UFS_RESET: 0x0f000000 + 0x1de000 = 0x0f1de000,
         matching Linux's TLMM 0x0f100000 + UFS_RESET 0xde000. */
      (*GpioContext)->GpioPins[i].PinAddress =
          (*GpioContext)->TlmmBaseAddress;
    }
  }

  return CR_SUCCESS;
}
