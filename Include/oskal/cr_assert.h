/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#include <oskal/cr_debug.h>

// For Windows Kernel Mode Drivers
#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <assert.h>

#define CR_ASSERT(exp)                                                         \
  do {                                                                         \
    if (!(exp)) {                                                              \
      ASSERT(exp);                                                             \
      KdPrintEx((DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,                      \
                 "%s fail %s(%d): %s\\n", __FUNCTION__, __FILE__, __LINE__,    \
                 #exp));                                                       \
      DbgBreakPoint();                                                         \
    }                                                                          \
  } while (0)

// Static assert
#define CR_STATIC_ASSERT(cond, msg) static_assert(cond, msg)

#else

// For UEFI Drivers
#include <Uefi.h>

#define CR_ASSERT(exp)                                                         \
  do {                                                                         \
    ASSERT(exp);                                                               \
    while (!(exp)) {                                                           \
      /* WFE mode - Wait For Event */                                          \
      asm volatile("wfe");                                                     \
    }                                                                          \
  } while (0)

// Static assert
#define CR_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif // _KERNEL_MODE
