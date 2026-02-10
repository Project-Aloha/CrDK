/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

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