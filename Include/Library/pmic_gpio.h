/** @file
 *  Portable Qualcomm SPMI PMIC GPIO controller core.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <Library/CrTargetPmicGpioLib.h>
#include <oskal/cr_interrupt.h>
#include <oskal/cr_lock.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define PMIC_GPIO_MAX_CONTROLLERS 8
#define PMIC_GPIO_MAX_PINS 36

typedef CR_STATUS (*PMIC_GPIO_BUS_READ)(
    IN VOID *Context, IN UINT8 Sid, IN UINT16 Address, OUT UINT8 *Buffer,
    IN UINTN Length);
typedef CR_STATUS (*PMIC_GPIO_BUS_WRITE)(
    IN VOID *Context, IN UINT8 Sid, IN UINT16 Address, IN CONST UINT8 *Buffer,
    IN UINTN Length);
typedef CR_STATUS (*PMIC_GPIO_BUS_REGISTER_IRQ)(
    IN VOID *Context, IN UINT8 Sid, IN UINT8 Peripheral, IN UINT8 Irq,
    IN CR_INTERRUPT_TRIGGER_TYPE Trigger, IN CR_INTERRUPT_HANDLER Handler,
    IN VOID *HandlerContext);
typedef CR_STATUS (*PMIC_GPIO_BUS_UNREGISTER_IRQ)(
    IN VOID *Context, IN UINT8 Sid, IN UINT8 Peripheral, IN UINT8 Irq);
typedef CR_STATUS (*PMIC_GPIO_BUS_ENABLE_IRQ)(
    IN VOID *Context, IN UINT8 Sid, IN UINT8 Peripheral, IN UINT8 Irq,
    IN BOOLEAN Enable);

typedef struct {
  PMIC_GPIO_BUS_READ           Read;
  PMIC_GPIO_BUS_WRITE          Write;
  PMIC_GPIO_BUS_REGISTER_IRQ   RegisterIrq;
  PMIC_GPIO_BUS_UNREGISTER_IRQ UnregisterIrq;
  PMIC_GPIO_BUS_ENABLE_IRQ     EnableIrq;
  VOID                        *Context;
} PmicGpioBusOps;

typedef enum {
  PMIC_GPIO_DIRECTION_INPUT = 0,
  PMIC_GPIO_DIRECTION_OUTPUT,
  PMIC_GPIO_DIRECTION_INPUT_OUTPUT,
  PMIC_GPIO_DIRECTION_ANALOG_PASS
} PmicGpioDirection;

typedef enum {
  PMIC_GPIO_PULL_UP_30UA = 0,
  PMIC_GPIO_PULL_UP_1P5UA,
  PMIC_GPIO_PULL_UP_31P5UA,
  PMIC_GPIO_PULL_UP_1P5UA_30UA_BOOST,
  PMIC_GPIO_PULL_DOWN_10UA,
  PMIC_GPIO_PULL_NONE
} PmicGpioPull;

typedef enum {
  PMIC_GPIO_BUFFER_PUSH_PULL = 0,
  PMIC_GPIO_BUFFER_OPEN_DRAIN,
  PMIC_GPIO_BUFFER_OPEN_SOURCE
} PmicGpioBufferType;

typedef enum {
  PMIC_GPIO_STRENGTH_NONE = 0,
  PMIC_GPIO_STRENGTH_LOW,
  PMIC_GPIO_STRENGTH_MEDIUM,
  PMIC_GPIO_STRENGTH_HIGH
} PmicGpioDriveStrength;

typedef enum {
  PMIC_GPIO_FUNCTION_NORMAL = 0,
  PMIC_GPIO_FUNCTION_PAIRED,
  PMIC_GPIO_FUNCTION_1,
  PMIC_GPIO_FUNCTION_2,
  PMIC_GPIO_FUNCTION_3,
  PMIC_GPIO_FUNCTION_4,
  PMIC_GPIO_FUNCTION_DTEST1,
  PMIC_GPIO_FUNCTION_DTEST2,
  PMIC_GPIO_FUNCTION_DTEST3,
  PMIC_GPIO_FUNCTION_DTEST4
} PmicGpioFunction;

typedef struct {
  BOOLEAN               Enabled;
  PmicGpioDirection     Direction;
  BOOLEAN               OutputValue;
  UINT8                 PowerSource;
  PmicGpioPull          Pull;
  PmicGpioBufferType    BufferType;
  PmicGpioDriveStrength DriveStrength;
  PmicGpioFunction      Function;
  UINT8                 Atest;
  UINT8                 DtestBuffer;
} PmicGpioConfig;

typedef struct {
  UINT16  Address;
  UINT8   Type;
  UINT8   Subtype;
  BOOLEAN Enabled;
  BOOLEAN OutputValue;
  BOOLEAN BufferSupported;
  BOOLEAN InputEnabled;
  BOOLEAN OutputEnabled;
  BOOLEAN AnalogPass;
  BOOLEAN LvMvType;
  UINT8   NumSources;
  UINT8   PowerSource;
  UINT8   BufferType;
  UINT8   Pull;
  UINT8   DriveStrength;
  UINT8   Function;
  UINT8   Atest;
  UINT8   DtestBuffer;
} PmicGpioPinState;

typedef struct {
  CONST PmicGpioTargetController *Target;
  PmicGpioPinState                Pins[PMIC_GPIO_MAX_PINS];
  BOOLEAN                         Initialized;
} PmicGpioControllerState;

typedef struct {
  CONST PmicGpioTargetContext *Target;
  PmicGpioBusOps               Bus;
  PmicGpioControllerState      Controllers[PMIC_GPIO_MAX_CONTROLLERS];
  CR_LOCK                      Lock;
  BOOLEAN                      Initialized;
} PmicGpioDeviceContext;

CR_STATUS
PmicGpioInitialize(
    OUT PmicGpioDeviceContext *Context, IN CONST PmicGpioTargetContext *Target,
    IN CONST PmicGpioBusOps *Bus);

CR_STATUS
PmicGpioLibInit(
    OUT PmicGpioDeviceContext *Context, IN CONST PmicGpioBusOps *Bus);

UINT16
PmicGpioGetControllerCount(IN CONST PmicGpioDeviceContext *Context);

UINT16
PmicGpioGetPinCount(
    IN CONST PmicGpioDeviceContext *Context, IN UINT16 Controller);

CR_STATUS
PmicGpioGetConfig(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    OUT PmicGpioConfig *Config);

CR_STATUS
PmicGpioConfigure(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN CONST PmicGpioConfig *Config);

CR_STATUS
PmicGpioGetValue(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    OUT BOOLEAN *Value);

CR_STATUS
PmicGpioSetValue(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN BOOLEAN Value);

CR_STATUS
PmicGpioSetDirection(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN PmicGpioDirection Direction);

CR_STATUS
PmicGpioSetPull(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN PmicGpioPull Pull);

CR_STATUS
PmicGpioRegisterInterrupt(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN CR_INTERRUPT_TRIGGER_TYPE Trigger, IN CR_INTERRUPT_HANDLER Handler,
    IN VOID *HandlerContext);

CR_STATUS
PmicGpioUnregisterInterrupt(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin);

CR_STATUS
PmicGpioEnableInterrupt(
    IN OUT PmicGpioDeviceContext *Context, IN UINT16 Controller, IN UINT16 Pin,
    IN BOOLEAN Enable);
