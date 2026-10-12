/* Board-selected public FocalTech firmware protocol. MIT */
#pragma once
#include <Library/gpio.h>
typedef struct {CONST CHAR8 *Name;UINT32 Millivolts;UINT8 Mode;} CR_TOUCH_RAIL;
typedef struct {
  UINT32 Revision,Size,I2cInstance;
  UINT8 Address,MaxContacts,ChipIdHigh,ChipIdLow;
  UINT16 XMax,YMax;
  UINT32 PollPeriodUs;
  GpioConfigParams Reset,Interrupt;
  CR_TOUCH_RAIL Rails[2]; /* VDD followed by VCC_I2C. */
} CR_TARGET_TOUCH_CONTEXT;
CONST CR_TARGET_TOUCH_CONTEXT *CrTargetGetTouchContext(VOID);
