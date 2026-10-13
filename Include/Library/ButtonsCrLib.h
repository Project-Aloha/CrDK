/** @file
  Portable PMIC button input, debounce and bounded navigation queue.
  SPDX-License-Identifier: MIT
**/
#pragma once

#include <Library/CrTargetButtonsLib.h>
#include <Library/pmic_gpio.h>
#include <Library/spmi.h>
#include <Library/SpmiSharedCrLib.h>

#define CR_BUTTON_QUEUE_SIZE 16

typedef VOID (*CR_BUTTON_WAKE)(VOID *Context);
typedef VOID (*CR_BUTTON_KEY_READY)(VOID *Context, BUTTON_CODE Code);

typedef struct {
  CONST ButtonTarget *Target;
  CR_SPMI_IRQ SharedIrq;
  UINT16 Controller;
  PmicGpioConfig SavedGpio;
  UINT8 SavedIrq[4];
  UINT32 SavedPic;
  BOOLEAN IrqSaved;
  BOOLEAN IrqRegistered;
  BOOLEAN GpioSaved;
  BOOLEAN Stable;
  BOOLEAN Candidate;
  BOOLEAN Armed;
  BOOLEAN PhysicalKnown, PhysicalPressed;
  UINT64 Since;
  UINT64 RepeatAt;
} CR_BUTTON_STATE;

typedef struct {
  CONST ButtonsTargetContext *Target;
  SpmiDeviceContext *Spmi;
  CR_SPMI_BUS *SharedBus;
  PmicGpioDeviceContext *Gpio;
  CR_BUTTON_WAKE Wake;
  CR_BUTTON_KEY_READY KeyReady;
  VOID *CallbackContext;
  CR_BUTTON_STATE State[BUTTON_CODE_MAX];
  BUTTON_CODE Queue[CR_BUTTON_QUEUE_SIZE];
  UINT16 Head;
  UINT16 Count;
  volatile UINT32 Dirty;
  UINT64 PowerPressSequence;
  BOOLEAN Initialized;
  BOOLEAN Stopping;
  BOOLEAN IoFailed;
} CR_BUTTON_CONTEXT;

/* Serialize service/read/reset/stop against each other. The native adapter
   raises TPL_HIGH_LEVEL around SPMI operations, so its IRQ cannot interrupt
   a held SPMI spin lock. Kernel adapters use the OSKAL deferred IRQ backend
   and serialize their power/worker paths. Wake is IRQ-safe; KeyReady runs
   in the serialized service path and must not reenter this library. */
CR_STATUS CrButtonsStart(CR_BUTTON_CONTEXT *Context,
                        CONST ButtonsTargetContext *Target,
                        SpmiDeviceContext *Spmi, PmicGpioDeviceContext *Gpio,
                        CR_BUTTON_WAKE Wake, CR_BUTTON_KEY_READY KeyReady,
                        VOID *CallbackContext, UINT64 NowUs);
CR_STATUS CrButtonsStop(CR_BUTTON_CONTEXT *Context);
CR_STATUS CrButtonsService(CR_BUTTON_CONTEXT *Context, UINT64 NowUs);
CR_STATUS CrButtonsReset(CR_BUTTON_CONTEXT *Context, UINT64 NowUs);
CR_STATUS CrButtonsRead(CR_BUTTON_CONTEXT *Context, BUTTON_CODE *Code);
BOOLEAN CrButtonsHasKey(CONST CR_BUTTON_CONTEXT *Context);
/* Zero means idle: wait for a PMIC interrupt, with no status polling. */
UINT32 CrButtonsNextServiceUs(CONST CR_BUTTON_CONTEXT *Context);

/* Read all physical levels without consuming keys or changing debounce. Caller
   uses the same serialized SPMI/IRQ exclusion as Service. Failed output is unchanged. */
CR_STATUS CrButtonsGetPressed(CR_BUTTON_CONTEXT *Context, UINT32 *PressedMask);
/* Same serialized current sample plus retained physical press history. Neither
   consumes the navigation queue; Reset never rewinds the sequence. */
CR_STATUS CrButtonsGetSnapshot(CR_BUTTON_CONTEXT *Context,UINT32 *PressedMask,UINT64 *PowerPressSequence);

/* Shared native arbiter: Bus retains its original leased address until Stop
   succeeds. GPIO bus must use the same client and serialization. */
CR_STATUS CrButtonsStartShared(CR_BUTTON_CONTEXT *Context,
                        CONST ButtonsTargetContext *Target,
                        CR_SPMI_BUS *Bus, PmicGpioDeviceContext *Gpio,
                        CR_BUTTON_WAKE Wake, CR_BUTTON_KEY_READY KeyReady,
                        VOID *CallbackContext, UINT64 NowUs);
