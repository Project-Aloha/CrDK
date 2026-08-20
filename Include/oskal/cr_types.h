/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#include "cr_assert.h"

#define TO_BOOL(x) ((x) ? TRUE : FALSE)

#ifdef _KERNEL_MODE
#include <ntddk.h>
typedef SIZE_T UINTN;
typedef SSIZE_T INTN;
typedef CHAR CHAR8;
#define STATIC static
#else
#include <Uefi.h>
#endif

CR_STATIC_ASSERT(sizeof(INTN) == sizeof(void *),
                 "INTN must be same size as pointer");
CR_STATIC_ASSERT(sizeof(UINTN) == sizeof(void *),
                 "UINTN must be same size as pointer");
CR_STATIC_ASSERT(sizeof(CHAR8) == 1, "CHAR8 must be 1 byte");
CR_STATIC_ASSERT(sizeof(INT16) == 2, "INT16 must be 2 bytes");
CR_STATIC_ASSERT(sizeof(UINT16) == 2, "UINT16 must be 2 bytes");
CR_STATIC_ASSERT(sizeof(INT32) == 4, "INT32 must be 4 bytes");
CR_STATIC_ASSERT(sizeof(UINT32) == 4, "UINT32 must be 4 bytes");
CR_STATIC_ASSERT(sizeof(INT64) == 8, "INT64 must be 8 bytes");
CR_STATIC_ASSERT(sizeof(UINT64) == 8, "UINT64 must be 8 bytes");
