/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/common.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <oskal/cr_interrupt.h>

#define PDC_RANGE_COUNT_MAX 32

typedef struct {
  UINT16 PdcPinBase;
  UINT16 GicSpiBase;
  UINT16 PdcCount;
} PdcRange;

typedef struct {
  UINTN BaseAddress;
  UINTN Size;
  UINTN SPIConfigBase;
  UINTN SPIConfigSize;
  UINT16 PdcRangeCount;
  PdcRange PdcRegion[PDC_RANGE_COUNT_MAX];
} PdcDeviceContext;

CR_STATUS
PdcLibInit(OUT PdcDeviceContext **CtxOut);

CR_STATUS PdcGicSetType(IN PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                        CR_INTERRUPT_TRIGGER_TYPE Type,
                        OUT CR_INTERRUPT_TRIGGER_TYPE *GicType);

CR_STATUS PdcGetGicIrqFromPdcPin(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                                 OUT UINT16 *GicIrq);

CR_STATUS
PdcRegisterInterrupt(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                     CR_INTERRUPT_HANDLER InterruptHandler, VOID *Param,
                     CR_INTERRUPT_TRIGGER_TYPE Type);

CR_STATUS
PdcUnregisterInterrupt(PdcDeviceContext *Ctx, UINT16 PdcPinNumber,
                       CR_INTERRUPT_TRIGGER_TYPE Type);

CR_STATUS PdcEnableInterrupt(IN PdcDeviceContext *Ctx, IN UINT16 PdcPinNumber,
                             BOOLEAN Enable);

CR_STATUS PdcGetPdcPinFromGicIrq(PdcDeviceContext *Ctx, UINT16 GicIrq,
                                 OUT UINT16 *PdcPinNumber);

CR_STATUS
PdcCheckInterruptEnabled(IN PdcDeviceContext *Ctx, IN UINT16 PdcPinNumber,
                         BOOLEAN *Enabled);