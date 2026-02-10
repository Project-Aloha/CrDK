/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/common.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>

#define DEBUG_UART_INPUT_BUF_SIZE 256

typedef enum {
  DEBUG_UART_TYPE_GENI = 0,
  DEBUG_UART_TYPE_DM,
} DEBUG_UART_TYPE;

typedef struct {
  UINTN   BaseAddress;
  UINTN   InterruptNo;
  BOOLEAN IsDebugUart; // TRUE if using inited debug uart by previous boot stage
  DEBUG_UART_TYPE Type;
  UINT32          ClockFrequency;
  UINT32          BaudRate;
  UINT8           DataBits;
  UINT8           Parity;
  UINT8           StopBits;
  UINT32          TxFifoDepth;
  UINT32          TxFifoWidth;
  UINT32          RxFifoDepth;
  BOOLEAN         TxComplete;

  VOID *InputBuffer;
  UINTN InputBufferSize;
  UINTN InputBufferHead;
  UINTN InputBufferTail;
  UINTN InputBufferCapacity;
} CrDebugUartContext;

CR_STATUS CrDebugUartLibInit(CrDebugUartContext **DebugUartContext);
VOID      MsmGeniWriteTxFifo(
         CrDebugUartContext *DebugUartContext, VOID *Buffer, UINTN Length);