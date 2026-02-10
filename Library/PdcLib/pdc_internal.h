/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once
#include <Library/pdc.h>
#include <oskal/common.h>
#include <oskal/cr_debug.h>
#include <oskal/cr_memory.h>
#include <oskal/cr_status.h>
#include <oskal/cr_time.h>
#include <oskal/cr_types.h>

#define PDC_IRQ_ENABLE_BANK 0x10
#define PDC_IRQ_I_CFG 0x110

enum PdcIrqConfig {
  PDC_IRQ_LEVEL_LOW = 0b000,
  PDC_IRQ_EDGE_FALLING = 0b010,
  PDC_IRQ_LEVEL_HIGH = 0b100,
  PDC_IRQ_EDGE_RISING = 0b110,
  PDC_IRQ_EDGE_DUAL = 0b111,
};
