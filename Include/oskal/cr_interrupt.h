/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <oskal/cr_interrupt.h>

typedef enum {
  CR_INTERRUPT_TRIGGER_LEVEL_LOW,
  CR_INTERRUPT_TRIGGER_LEVEL_HIGH,
  CR_INTERRUPT_TRIGGER_EDGE_FALLING,
  CR_INTERRUPT_TRIGGER_EDGE_RISING,
  CR_INTERRUPT_TRIGGER_EDGE_BOTH,
  CR_INTERRUPT_TRIGGER_TYPE_INVALID = 0xFF
} CR_INTERRUPT_TRIGGER_TYPE;

typedef VOID (*CR_INTERRUPT_HANDLER)(VOID *Parameters);

typedef struct {
  CR_INTERRUPT_HANDLER Handler;
  VOID *Param;
  UINT64 Source;
} CR_INTERRUPT_ENTRY;
// 16 is enough for a single driver linked to this library.
#define MAX_INTERRUPT_ENTRIES 16

#ifdef _KERNEL_MODE
#include <wdf.h>
typedef enum {
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_NONE = 0,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_DPC,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_WORKITEM
} OSKAL_INTERRUPT_L_IRQ_LCB_TYPE;
typedef struct {
  VOID *Param;
  CR_INTERRUPT_HANDLER Handler;
  // Initialized WDF interrupt config by evt device prepare hardware function
  WDF_INTERRUPT_CONFIG WdfInterruptConfig;
  WDF_OBJECT_ATTRIBUTES WdfInterruptAttributes;
  // Interrupt handle
  WDFINTERRUPT Interrupt;
  // Device handle
  WDFDEVICE Device;
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE LIrqLCbType;
  WDFWORKITEM WorkItem;
} CR_INTERRUPT_CONFIG;
#else

enum {
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_NONE = 0,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_DPC,
  OSKAL_INTERRUPT_L_IRQ_LCB_TYPE_WORKITEM
};

typedef struct {
  VOID *Param;
  CR_INTERRUPT_HANDLER Handler;
  UINT32 InterruptNumber;
  CR_INTERRUPT_TRIGGER_TYPE TriggerType;
} CR_INTERRUPT_CONFIG;
#endif

/**
 * Register an interrupt handler for a specific interrupt vector
 *
 * @param InterruptNumber  The interrupt vector number to register
 * @param InterruptHandler The handler function to call when interrupt fires,
 * disable irq if null.
 * @param Param            Parameter to pass to the interrupt handler
 * @param TriggerType      Interrupt trigger type (edge or level)
 *
 * @return CR_SUCCESS if successful, error code otherwise
 */
CR_STATUS
CrRegisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig);

/**
 * Unregister an interrupt handler
 *
 * @param InterruptNumber  The interrupt vector number to unregister
 *
 * @return CR_SUCCESS if successful, error code otherwise
 */
CR_STATUS
CrUnregisterInterrupt(CR_INTERRUPT_CONFIG *InterruptConfig);
