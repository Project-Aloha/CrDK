/** @file
 *  Portable target-data ABI for physical button descriptions.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/cr_interrupt.h>
#include <oskal/cr_types.h>

typedef enum {
  BUTTON_SOURCE_PON,
  BUTTON_SOURCE_PMIC_GPIO
} BUTTON_SOURCE;

typedef enum {
  BUTTON_CODE_POWER,
  BUTTON_CODE_VOLUME_UP,
  BUTTON_CODE_VOLUME_DOWN,
  BUTTON_CODE_MAX
} BUTTON_CODE;

typedef struct {
  CONST CHAR8              *Name;
  BUTTON_SOURCE             Source;
  BUTTON_CODE               Code;
  CONST CHAR8              *Controller;
  UINT8                     Sid;
  UINT8                     Peripheral;
  UINT16                    Pin;
  UINT8                     Irq;
  BOOLEAN                   ActiveLow;
  UINT32                    DebounceUs;
  CR_INTERRUPT_TRIGGER_TYPE Trigger;
} ButtonTarget;

typedef struct {
  CONST ButtonTarget *Buttons;
  UINT16              ButtonCount;
} ButtonsTargetContext;

ButtonsTargetContext *
CrTargetGetButtonsContext(VOID);
