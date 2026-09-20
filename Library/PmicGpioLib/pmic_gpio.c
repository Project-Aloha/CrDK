/** @file
 *  Qualcomm SPMI PMIC GPIO controller core, based on Linux
 *  drivers/pinctrl/qcom/pinctrl-spmi-gpio.c register semantics.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Library/pmic_gpio.h>
#include <oskal/common.h>
#include <oskal/cr_string.h>

#define PMIC_GPIO_ADDRESS_RANGE 0x100

#define PMIC_GPIO_REG_TYPE 0x04
#define PMIC_GPIO_REG_SUBTYPE 0x05
#define PMIC_GPIO_REG_RT_STS 0x10
#define PMIC_GPIO_REG_MODE_CTL 0x40
#define PMIC_GPIO_REG_DIG_VIN_CTL 0x41
#define PMIC_GPIO_REG_DIG_PULL_CTL 0x42
#define PMIC_GPIO_REG_DIG_IN_CTL 0x43
#define PMIC_GPIO_REG_LV_MV_DIG_OUT_SOURCE_CTL 0x44
#define PMIC_GPIO_REG_DIG_OUT_CTL 0x45
#define PMIC_GPIO_REG_EN_CTL 0x46
#define PMIC_GPIO_REG_LV_MV_ANA_PASS_THRU_SEL 0x4A

#define PMIC_GPIO_TYPE 0x10
#define PMIC_GPIO_SUBTYPE_GPIO_4CH 0x01
#define PMIC_GPIO_SUBTYPE_GPIOC_4CH 0x05
#define PMIC_GPIO_SUBTYPE_GPIO_8CH 0x09
#define PMIC_GPIO_SUBTYPE_GPIOC_8CH 0x0D
#define PMIC_GPIO_SUBTYPE_GPIO_LV 0x10
#define PMIC_GPIO_SUBTYPE_GPIO_MV 0x11
#define PMIC_GPIO_SUBTYPE_GPIO_LV_VIN2 0x12
#define PMIC_GPIO_SUBTYPE_GPIO_MV_VIN3 0x13
#define PMIC_GPIO_SUBTYPE_GPIO_LV_VIN2_CLK 0x14
#define PMIC_GPIO_SUBTYPE_GPIO_MV_VIN3_CLK 0x15

#define PMIC_GPIO_RT_STS_VALUE BIT(0)
#define PMIC_GPIO_MODE_VALUE BIT(0)
#define PMIC_GPIO_MODE_FUNCTION_SHIFT 1
#define PMIC_GPIO_MODE_FUNCTION_MASK 0x7
#define PMIC_GPIO_MODE_DIRECTION_SHIFT 4
#define PMIC_GPIO_MODE_DIRECTION_MASK 0x7
#define PMIC_GPIO_LV_MV_MODE_DIRECTION_MASK 0x3
#define PMIC_GPIO_VIN_MASK 0x7
#define PMIC_GPIO_PULL_MASK 0x7
#define PMIC_GPIO_LV_MV_OUTPUT_INVERT BIT(7)
#define PMIC_GPIO_LV_MV_OUTPUT_SOURCE_MASK 0xF
#define PMIC_GPIO_LV_MV_DIG_IN_DTEST_ENABLE BIT(7)
#define PMIC_GPIO_LV_MV_DIG_IN_DTEST_MASK 0x7
#define PMIC_GPIO_DIG_IN_DTEST_MASK 0xF
#define PMIC_GPIO_OUT_STRENGTH_MASK 0x3
#define PMIC_GPIO_OUT_TYPE_SHIFT 4
#define PMIC_GPIO_OUT_TYPE_MASK 0x3
#define PMIC_GPIO_MASTER_ENABLE BIT(7)
#define PMIC_GPIO_LV_MV_ANA_MUX_MASK 0x3

STATIC CR_STATUS ReadByte(
    IN PmicGpioDeviceContext *Context, IN UINT8 Sid, IN UINT16 Address,
    OUT UINT8 *Value)
{
  return Context->Bus.Read(Context->Bus.Context, Sid, Address, Value, 1);
}

STATIC CR_STATUS WriteByte(
    IN PmicGpioDeviceContext *Context, IN UINT8 Sid, IN UINT16 Address,
    IN UINT8 Value)
{
  return Context->Bus.Write(Context->Bus.Context, Sid, Address, &Value, 1);
}

STATIC UINT8 FirstSetBit(IN UINT8 Value)
{
  UINT8 Index;

  for (Index = 0; Index < 8; Index++) {
    if ((Value & (UINT8)BIT(Index)) != 0) {
      return (UINT8)(Index + 1);
    }
  }
  return 0;
}

STATIC CR_STATUS LocatePin(
    IN PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    OUT PmicGpioControllerState **ControllerState,
    OUT PmicGpioPinState        **PinState)
{
  if (Context == NULL || !Context->Initialized ||
      Controller >= Context->Target->ControllerCount || Pin == 0 ||
      Pin > Context->Controllers[Controller].Target->PinCount) {
    return CR_INVALID_PARAMETER;
  }

  *ControllerState = &Context->Controllers[Controller];
  *PinState        = &Context->Controllers[Controller].Pins[Pin - 1];
  return CR_SUCCESS;
}

STATIC VOID
StateToConfig(IN CONST PmicGpioPinState *State, OUT PmicGpioConfig *Config)
{
  Config->Enabled = State->Enabled;
  if (State->AnalogPass) {
    Config->Direction = PMIC_GPIO_DIRECTION_ANALOG_PASS;
  }
  else if (State->InputEnabled && State->OutputEnabled) {
    Config->Direction = PMIC_GPIO_DIRECTION_INPUT_OUTPUT;
  }
  else if (State->OutputEnabled) {
    Config->Direction = PMIC_GPIO_DIRECTION_OUTPUT;
  }
  else {
    Config->Direction = PMIC_GPIO_DIRECTION_INPUT;
  }
  Config->OutputValue   = State->OutputValue;
  Config->PowerSource   = State->PowerSource;
  Config->Pull          = (PmicGpioPull)State->Pull;
  Config->BufferType    = (PmicGpioBufferType)State->BufferType;
  Config->DriveStrength = (PmicGpioDriveStrength)State->DriveStrength;
  Config->Function      = (PmicGpioFunction)State->Function;
  Config->Atest         = State->Atest;
  Config->DtestBuffer   = State->DtestBuffer;
}

STATIC CR_STATUS ReadPinState(
    IN OUT PmicGpioDeviceContext *Context,
    IN PmicGpioControllerState *Controller, IN OUT PmicGpioPinState *Pin)
{
  CR_STATUS Status;
  UINT8     Sid;
  UINT8     Value;
  UINT8     Direction;

  Sid    = Controller->Target->Sid;
  Status = ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_TYPE, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Pin->Type = Value;
  if (Value != PMIC_GPIO_TYPE) {
    return CR_DEVICE_ERROR;
  }

  Status = ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_SUBTYPE, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Pin->Subtype         = Value;
  Pin->BufferSupported = FALSE;
  Pin->LvMvType        = FALSE;
  switch (Value) {
  case PMIC_GPIO_SUBTYPE_GPIO_4CH:
    Pin->BufferSupported = TRUE;
    Pin->NumSources      = 4;
    break;
  case PMIC_GPIO_SUBTYPE_GPIOC_4CH:
    Pin->NumSources = 4;
    break;
  case PMIC_GPIO_SUBTYPE_GPIO_8CH:
    Pin->BufferSupported = TRUE;
    Pin->NumSources      = 8;
    break;
  case PMIC_GPIO_SUBTYPE_GPIOC_8CH:
    Pin->NumSources = 8;
    break;
  case PMIC_GPIO_SUBTYPE_GPIO_LV:
    Pin->NumSources      = 1;
    Pin->BufferSupported = TRUE;
    Pin->LvMvType        = TRUE;
    break;
  case PMIC_GPIO_SUBTYPE_GPIO_MV:
  case PMIC_GPIO_SUBTYPE_GPIO_LV_VIN2:
  case PMIC_GPIO_SUBTYPE_GPIO_LV_VIN2_CLK:
    Pin->NumSources      = 2;
    Pin->BufferSupported = TRUE;
    Pin->LvMvType        = TRUE;
    break;
  case PMIC_GPIO_SUBTYPE_GPIO_MV_VIN3:
  case PMIC_GPIO_SUBTYPE_GPIO_MV_VIN3_CLK:
    Pin->NumSources      = 3;
    Pin->BufferSupported = TRUE;
    Pin->LvMvType        = TRUE;
    break;
  default:
    return CR_UNSUPPORTED;
  }

  if (Pin->LvMvType) {
    Status = ReadByte(
        Context, Sid, Pin->Address + PMIC_GPIO_REG_LV_MV_DIG_OUT_SOURCE_CTL,
        &Value);
    if (CR_ERROR(Status)) {
      return Status;
    }
    Pin->OutputValue = TO_BOOL(Value & PMIC_GPIO_LV_MV_OUTPUT_INVERT);
    Pin->Function    = Value & PMIC_GPIO_LV_MV_OUTPUT_SOURCE_MASK;
  }

  Status =
      ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_MODE_CTL, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Pin->LvMvType) {
    Direction = Value & PMIC_GPIO_LV_MV_MODE_DIRECTION_MASK;
  }
  else {
    Pin->OutputValue = TO_BOOL(Value & PMIC_GPIO_MODE_VALUE);
    Direction        = (Value >> PMIC_GPIO_MODE_DIRECTION_SHIFT) &
                       PMIC_GPIO_MODE_DIRECTION_MASK;
    Pin->Function =
        (Value >> PMIC_GPIO_MODE_FUNCTION_SHIFT) & PMIC_GPIO_MODE_FUNCTION_MASK;
    if (Pin->Function >= PMIC_GPIO_FUNCTION_3) {
      Pin->Function += PMIC_GPIO_FUNCTION_DTEST1 - PMIC_GPIO_FUNCTION_3;
    }
  }

  Pin->InputEnabled  = FALSE;
  Pin->OutputEnabled = FALSE;
  Pin->AnalogPass    = FALSE;
  switch (Direction) {
  case PMIC_GPIO_DIRECTION_INPUT:
    Pin->InputEnabled = TRUE;
    break;
  case PMIC_GPIO_DIRECTION_OUTPUT:
    Pin->OutputEnabled = TRUE;
    break;
  case PMIC_GPIO_DIRECTION_INPUT_OUTPUT:
    Pin->InputEnabled  = TRUE;
    Pin->OutputEnabled = TRUE;
    break;
  case PMIC_GPIO_DIRECTION_ANALOG_PASS:
    if (!Pin->LvMvType) {
      return CR_DEVICE_ERROR;
    }
    Pin->AnalogPass = TRUE;
    break;
  default:
    return CR_DEVICE_ERROR;
  }

  Status =
      ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_VIN_CTL, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Pin->PowerSource = Value & PMIC_GPIO_VIN_MASK;

  Status =
      ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_PULL_CTL, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Pin->Pull = Value & PMIC_GPIO_PULL_MASK;

  Status =
      ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_IN_CTL, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Pin->LvMvType && ((Value & PMIC_GPIO_LV_MV_DIG_IN_DTEST_ENABLE) != 0)) {
    Pin->DtestBuffer = (Value & PMIC_GPIO_LV_MV_DIG_IN_DTEST_MASK) + 1;
  }
  else if (!Pin->LvMvType) {
    Pin->DtestBuffer = FirstSetBit(Value & PMIC_GPIO_DIG_IN_DTEST_MASK);
  }
  else {
    Pin->DtestBuffer = 0;
  }

  Status =
      ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_OUT_CTL, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Pin->DriveStrength = Value & PMIC_GPIO_OUT_STRENGTH_MASK;
  Pin->BufferType =
      (Value >> PMIC_GPIO_OUT_TYPE_SHIFT) & PMIC_GPIO_OUT_TYPE_MASK;

  if (Pin->LvMvType) {
    Status = ReadByte(
        Context, Sid, Pin->Address + PMIC_GPIO_REG_LV_MV_ANA_PASS_THRU_SEL,
        &Value);
    if (CR_ERROR(Status)) {
      return Status;
    }
    Pin->Atest = (Value & PMIC_GPIO_LV_MV_ANA_MUX_MASK) + 1;
  }
  else {
    Pin->Atest = 0;
  }

  Status = ReadByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_EN_CTL, &Value);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Pin->Enabled = TO_BOOL(Value & PMIC_GPIO_MASTER_ENABLE);
  return CR_SUCCESS;
}

STATIC CR_STATUS ValidateConfig(
    IN CONST PmicGpioPinState *Pin, IN CONST PmicGpioConfig *Config,
    OUT UINT8 *HardwareFunction)
{
  if (Config->Direction > PMIC_GPIO_DIRECTION_ANALOG_PASS ||
      Config->Pull > PMIC_GPIO_PULL_NONE ||
      Config->BufferType > PMIC_GPIO_BUFFER_OPEN_SOURCE ||
      Config->DriveStrength > PMIC_GPIO_STRENGTH_HIGH ||
      Config->Function > PMIC_GPIO_FUNCTION_DTEST4 ||
      Config->PowerSource >= Pin->NumSources || Config->DtestBuffer > 4) {
    return CR_INVALID_PARAMETER;
  }
  if (Config->BufferType != PMIC_GPIO_BUFFER_PUSH_PULL &&
      !Pin->BufferSupported) {
    return CR_UNSUPPORTED;
  }
  if (Config->Direction == PMIC_GPIO_DIRECTION_ANALOG_PASS && !Pin->LvMvType) {
    return CR_UNSUPPORTED;
  }
  if (Pin->LvMvType) {
    if (Config->Atest < 1 || Config->Atest > 4) {
      return CR_INVALID_PARAMETER;
    }
    *HardwareFunction = (UINT8)Config->Function;
  }
  else {
    if (Config->Function == PMIC_GPIO_FUNCTION_3 ||
        Config->Function == PMIC_GPIO_FUNCTION_4) {
      return CR_UNSUPPORTED;
    }
    *HardwareFunction = (UINT8)Config->Function;
    if (Config->Function >= PMIC_GPIO_FUNCTION_DTEST1) {
      *HardwareFunction -= PMIC_GPIO_FUNCTION_DTEST1 - PMIC_GPIO_FUNCTION_3;
    }
  }
  return CR_SUCCESS;
}

STATIC CR_STATUS ApplyConfig(
    IN OUT PmicGpioDeviceContext *Context,
    IN PmicGpioControllerState *Controller, IN OUT PmicGpioPinState *Pin,
    IN CONST PmicGpioConfig *Config)
{
  CR_STATUS Status;
  UINT8     Sid;
  UINT8     Value;
  UINT8     HardwareFunction;

  Status = ValidateConfig(Pin, Config, &HardwareFunction);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Sid = Controller->Target->Sid;

  Status = WriteByte(
      Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_VIN_CTL,
      Config->PowerSource & PMIC_GPIO_VIN_MASK);
  if (CR_ERROR(Status)) {
    goto RefreshState;
  }
  Status = WriteByte(
      Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_PULL_CTL,
      (UINT8)Config->Pull & PMIC_GPIO_PULL_MASK);
  if (CR_ERROR(Status)) {
    goto RefreshState;
  }

  Value = ((UINT8)Config->BufferType << PMIC_GPIO_OUT_TYPE_SHIFT) |
          ((UINT8)Config->DriveStrength & PMIC_GPIO_OUT_STRENGTH_MASK);
  Status =
      WriteByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_OUT_CTL, Value);
  if (CR_ERROR(Status)) {
    goto RefreshState;
  }

  if (Config->DtestBuffer == 0) {
    Value = 0;
  }
  else if (Pin->LvMvType) {
    Value =
        (UINT8)(Config->DtestBuffer - 1) | PMIC_GPIO_LV_MV_DIG_IN_DTEST_ENABLE;
  }
  else {
    Value = (UINT8)BIT(Config->DtestBuffer - 1);
  }
  Status =
      WriteByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_DIG_IN_CTL, Value);
  if (CR_ERROR(Status)) {
    goto RefreshState;
  }

  Value = (UINT8)Config->Direction;
  if (Pin->LvMvType) {
    Status =
        WriteByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_MODE_CTL, Value);
    if (CR_ERROR(Status)) {
      goto RefreshState;
    }
    Status = WriteByte(
        Context, Sid, Pin->Address + PMIC_GPIO_REG_LV_MV_ANA_PASS_THRU_SEL,
        (UINT8)(Config->Atest - 1));
    if (CR_ERROR(Status)) {
      goto RefreshState;
    }
    Value  = (Config->OutputValue ? PMIC_GPIO_LV_MV_OUTPUT_INVERT : 0) |
             (HardwareFunction & PMIC_GPIO_LV_MV_OUTPUT_SOURCE_MASK);
    Status = WriteByte(
        Context, Sid, Pin->Address + PMIC_GPIO_REG_LV_MV_DIG_OUT_SOURCE_CTL,
        Value);
  }
  else {
    Value = ((UINT8)Config->Direction << PMIC_GPIO_MODE_DIRECTION_SHIFT) |
            (HardwareFunction << PMIC_GPIO_MODE_FUNCTION_SHIFT) |
            (Config->OutputValue ? PMIC_GPIO_MODE_VALUE : 0);
    Status =
        WriteByte(Context, Sid, Pin->Address + PMIC_GPIO_REG_MODE_CTL, Value);
  }
  if (CR_ERROR(Status)) {
    goto RefreshState;
  }

  Status = WriteByte(
      Context, Sid, Pin->Address + PMIC_GPIO_REG_EN_CTL,
      Config->Enabled ? PMIC_GPIO_MASTER_ENABLE : 0);
  if (CR_ERROR(Status)) {
    goto RefreshState;
  }

  Pin->Enabled       = Config->Enabled;
  Pin->InputEnabled  = Config->Direction == PMIC_GPIO_DIRECTION_INPUT ||
                       Config->Direction == PMIC_GPIO_DIRECTION_INPUT_OUTPUT;
  Pin->OutputEnabled = Config->Direction == PMIC_GPIO_DIRECTION_OUTPUT ||
                       Config->Direction == PMIC_GPIO_DIRECTION_INPUT_OUTPUT;
  Pin->AnalogPass    = Config->Direction == PMIC_GPIO_DIRECTION_ANALOG_PASS;
  Pin->OutputValue   = Config->OutputValue;
  Pin->PowerSource   = Config->PowerSource;
  Pin->Pull          = (UINT8)Config->Pull;
  Pin->BufferType    = (UINT8)Config->BufferType;
  Pin->DriveStrength = (UINT8)Config->DriveStrength;
  Pin->Function      = (UINT8)Config->Function;
  Pin->Atest         = Config->Atest;
  Pin->DtestBuffer   = Config->DtestBuffer;
  return CR_SUCCESS;

RefreshState:
  (VOID) ReadPinState(Context, Controller, Pin);
  return Status;
}

CR_STATUS
PmicGpioInitialize(
    OUT PmicGpioDeviceContext *Context, IN CONST PmicGpioTargetContext *Target,
    IN CONST PmicGpioBusOps *Bus)
{
  CR_STATUS Status;
  UINT16    Controller;
  UINT16    Pin;
  UINT32    Address;

  if (Context == NULL || Target == NULL || Bus == NULL || Bus->Read == NULL ||
      Bus->Write == NULL || Target->Controllers == NULL ||
      Target->ControllerCount == 0 ||
      Target->ControllerCount > PMIC_GPIO_MAX_CONTROLLERS) {
    return CR_INVALID_PARAMETER;
  }
  cr_memset(Context, 0, sizeof(*Context));
  Context->Target = Target;
  Context->Bus    = *Bus;
  CrLockInit(&Context->Lock);

  for (Controller = 0; Controller < Target->ControllerCount; Controller++) {
    if (Target->Controllers[Controller].PinCount == 0 ||
        Target->Controllers[Controller].PinCount > PMIC_GPIO_MAX_PINS) {
      return CR_INVALID_PARAMETER;
    }
    Context->Controllers[Controller].Target = &Target->Controllers[Controller];
    for (Pin = 0; Pin < Target->Controllers[Controller].PinCount; Pin++) {
      Address = Target->Controllers[Controller].PeripheralBase +
                Pin * PMIC_GPIO_ADDRESS_RANGE;
      if (Address > 0xFFFF) {
        return CR_INVALID_PARAMETER;
      }
      Context->Controllers[Controller].Pins[Pin].Address = (UINT16)Address;
      Status                                             = ReadPinState(
          Context, &Context->Controllers[Controller],
          &Context->Controllers[Controller].Pins[Pin]);
      if (CR_ERROR(Status)) {
        return Status;
      }
    }
    Context->Controllers[Controller].Initialized = TRUE;
  }
  Context->Initialized = TRUE;
  return CR_SUCCESS;
}

CR_STATUS
PmicGpioLibInit(
    OUT PmicGpioDeviceContext *Context, IN CONST PmicGpioBusOps *Bus)
{
  PmicGpioTargetContext *Target;

  Target = CrTargetGetPmicGpioContext();
  if (Target == NULL) {
    return CR_NOT_FOUND;
  }
  return PmicGpioInitialize(Context, Target, Bus);
}

UINT16
PmicGpioGetControllerCount(IN CONST PmicGpioDeviceContext *Context)
{
  return Context != NULL && Context->Initialized
             ? Context->Target->ControllerCount
             : 0;
}

UINT16
PmicGpioGetPinCount(
    IN CONST PmicGpioDeviceContext *Context, IN UINT16 Controller)
{
  if (Context == NULL || !Context->Initialized ||
      Controller >= Context->Target->ControllerCount) {
    return 0;
  }
  return Context->Controllers[Controller].Target->PinCount;
}

CR_STATUS
PmicGpioGetConfig(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    OUT PmicGpioConfig *Config)
{
  PmicGpioControllerState *ControllerState;
  PmicGpioPinState        *PinState;
  CR_STATUS                Status;

  if (Config == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = LocatePin(Context, Controller, Pin, &ControllerState, &PinState);
  if (CR_ERROR(Status)) {
    return Status;
  }
  CrLockAcquire(&Context->Lock);
  Status = ReadPinState(Context, ControllerState, PinState);
  if (!CR_ERROR(Status)) {
    StateToConfig(PinState, Config);
  }
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS
PmicGpioConfigure(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN CONST PmicGpioConfig *Config)
{
  PmicGpioControllerState *ControllerState;
  PmicGpioPinState        *PinState;
  CR_STATUS                Status;

  if (Config == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = LocatePin(Context, Controller, Pin, &ControllerState, &PinState);
  if (CR_ERROR(Status)) {
    return Status;
  }
  CrLockAcquire(&Context->Lock);
  Status = ApplyConfig(Context, ControllerState, PinState, Config);
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS
PmicGpioGetValue(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    OUT BOOLEAN *Value)
{
  PmicGpioControllerState *ControllerState;
  PmicGpioPinState        *PinState;
  CR_STATUS                Status;
  UINT8                    RegisterValue;

  if (Value == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = LocatePin(Context, Controller, Pin, &ControllerState, &PinState);
  if (CR_ERROR(Status)) {
    return Status;
  }
  CrLockAcquire(&Context->Lock);
  if (!PinState->Enabled || PinState->AnalogPass) {
    Status = CR_DEVICE_ERROR;
  }
  else if (PinState->InputEnabled) {
    Status = ReadByte(
        Context, ControllerState->Target->Sid,
        PinState->Address + PMIC_GPIO_REG_RT_STS, &RegisterValue);
    if (!CR_ERROR(Status)) {
      PinState->OutputValue = TO_BOOL(RegisterValue & PMIC_GPIO_RT_STS_VALUE);
      *Value                = PinState->OutputValue;
    }
  }
  else {
    *Value = PinState->OutputValue;
    Status = CR_SUCCESS;
  }
  CrLockRelease(&Context->Lock);
  return Status;
}

CR_STATUS
PmicGpioSetValue(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN BOOLEAN Value)
{
  PmicGpioConfig Config;
  CR_STATUS      Status;

  Status = PmicGpioGetConfig(Context, Controller, Pin, &Config);
  if (CR_ERROR(Status)) {
    return Status;
  }
  if (Config.Direction == PMIC_GPIO_DIRECTION_ANALOG_PASS) {
    return CR_UNSUPPORTED;
  }
  if (Config.Direction == PMIC_GPIO_DIRECTION_INPUT) {
    Config.Direction = PMIC_GPIO_DIRECTION_INPUT_OUTPUT;
  }
  Config.Enabled     = TRUE;
  Config.OutputValue = TO_BOOL(Value);
  return PmicGpioConfigure(Context, Controller, Pin, &Config);
}

CR_STATUS
PmicGpioSetDirection(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN PmicGpioDirection Direction)
{
  PmicGpioConfig Config;
  CR_STATUS      Status;

  Status = PmicGpioGetConfig(Context, Controller, Pin, &Config);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Config.Enabled   = TRUE;
  Config.Direction = Direction;
  return PmicGpioConfigure(Context, Controller, Pin, &Config);
}

CR_STATUS
PmicGpioSetPull(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN PmicGpioPull Pull)
{
  PmicGpioConfig Config;
  CR_STATUS      Status;

  Status = PmicGpioGetConfig(Context, Controller, Pin, &Config);
  if (CR_ERROR(Status)) {
    return Status;
  }
  Config.Enabled = TRUE;
  Config.Pull    = Pull;
  return PmicGpioConfigure(Context, Controller, Pin, &Config);
}

STATIC CR_STATUS GetInterruptAddress(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    OUT UINT8 *Sid, OUT UINT8 *Peripheral)
{
  PmicGpioControllerState *ControllerState;
  PmicGpioPinState        *PinState;
  CR_STATUS                Status;

  Status = LocatePin(Context, Controller, Pin, &ControllerState, &PinState);
  if (CR_ERROR(Status)) {
    return Status;
  }
  *Sid        = ControllerState->Target->Sid;
  *Peripheral = (UINT8)(PinState->Address >> 8);
  return CR_SUCCESS;
}

CR_STATUS
PmicGpioRegisterInterrupt(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN CR_INTERRUPT_TRIGGER_TYPE Trigger, IN CR_INTERRUPT_HANDLER Handler,
    IN VOID *HandlerContext)
{
  CR_STATUS Status;
  UINT8     Sid;
  UINT8     Peripheral;

  if (Context == NULL || Context->Bus.RegisterIrq == NULL || Handler == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = GetInterruptAddress(Context, Controller, Pin, &Sid, &Peripheral);
  if (CR_ERROR(Status)) {
    return Status;
  }
  return Context->Bus.RegisterIrq(
      Context->Bus.Context, Sid, Peripheral, 0, Trigger, Handler,
      HandlerContext);
}

CR_STATUS
PmicGpioUnregisterInterrupt(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin)
{
  CR_STATUS Status;
  UINT8     Sid;
  UINT8     Peripheral;

  if (Context == NULL || Context->Bus.UnregisterIrq == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = GetInterruptAddress(Context, Controller, Pin, &Sid, &Peripheral);
  if (CR_ERROR(Status)) {
    return Status;
  }
  return Context->Bus.UnregisterIrq(Context->Bus.Context, Sid, Peripheral, 0);
}

CR_STATUS
PmicGpioEnableInterrupt(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN BOOLEAN Enable)
{
  CR_STATUS Status;
  UINT8     Sid;
  UINT8     Peripheral;

  if (Context == NULL || Context->Bus.EnableIrq == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = GetInterruptAddress(Context, Controller, Pin, &Sid, &Peripheral);
  if (CR_ERROR(Status)) {
    return Status;
  }
  return Context->Bus.EnableIrq(
      Context->Bus.Context, Sid, Peripheral, 0, Enable);
}
