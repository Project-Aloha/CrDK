/* FTS firmware reports and input/resource lifetime. SPDX-License-Identifier: GPL-2.0-only */
#pragma once
#include <Library/CrTargetTouchLib.h>
#define CR_FTS_REPORT_SIZE 33U
#define CR_FTS_QUEUE_CAPACITY 16U
typedef struct {UINT16 X,Y;BOOLEAN Pressed;} CR_FTS_STATE;
typedef struct {
  VOID *Context;
  CR_STATUS (*AcquirePin)(VOID *,VOID *Owner,CONST GpioConfigParams *,BOOLEAN *Owned);
  CR_STATUS (*SetReset)(VOID *,VOID *Owner,UINT16,UINT8);
  CR_STATUS (*ReleasePin)(VOID *,VOID *Owner,UINT16);
  CR_STATUS (*AcquireRail)(VOID *,CONST CR_TOUCH_RAIL *,BOOLEAN *Owned);
  CR_STATUS (*ReleaseRail)(VOID *,CONST CR_TOUCH_RAIL *);
  CR_STATUS (*Read)(VOID *,UINT8 Register,UINT8 *Data,UINT32 Length);
  VOID (*DelayUs)(VOID *,UINT32);
  CR_STATUS (*ArmReport)(VOID *,VOID *Owner,UINT16 Pin);
  CR_STATUS (*TakeReport)(VOID *,VOID *Owner,UINT16 Pin,BOOLEAN *Pending);
} CR_FTS_IO;
typedef struct {
  CONST CR_TARGET_TOUCH_CONTEXT *Target;
  CR_FTS_IO Io;
  BOOLEAN Initialized,Active,PinOwned[2],RailOwned[2],UncertainOwner;
  UINT8 Selected,Head,Count;
  CR_FTS_STATE Physical,Delivered,Queue[CR_FTS_QUEUE_CAPACITY];
  CR_STATUS LastError;
} CR_FTS_TOUCH;
/* Fixed caller-owned storage through every failed stop. No callback or target
 * pointer may be freed while any Owned flag remains. All calls serialized.
 * Polling consumes latched report IRQs only; no boot-ROM/firmware download.
 * The snapshot adapter must supply an exclusive native GPIO lease. */
CR_STATUS CrFtsInit(CR_FTS_TOUCH *,CONST CR_TARGET_TOUCH_CONTEXT *,CONST CR_FTS_IO *);
CR_STATUS CrFtsStart(CR_FTS_TOUCH *);
CR_STATUS CrFtsService(CR_FTS_TOUCH *);
CR_STATUS CrFtsGetState(CR_FTS_TOUCH *,CR_FTS_STATE *);
CR_STATUS CrFtsStop(CR_FTS_TOUCH *);
VOID CrFtsCancel(CR_FTS_TOUCH *,CR_STATUS);
