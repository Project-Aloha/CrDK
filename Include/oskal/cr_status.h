/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
// OS Independent status code definitions
#ifdef _KERNEL_MODE
#include <ntddk.h>

typedef NTSTATUS CR_STATUS;

/* Map common CR_* symbols to NTSTATUS equivalents */
#define CR_SUCCESS STATUS_SUCCESS
#define CR_ABORT STATUS_CANCELLED
#define CR_INVALID_PARAMETER STATUS_INVALID_PARAMETER
#define CR_NOT_FOUND STATUS_NOT_FOUND
#define CR_OUT_OF_RESOURCES STATUS_INSUFFICIENT_RESOURCES
#define CR_UNSUPPORTED STATUS_NOT_SUPPORTED
#define CR_DEVICE_ERROR STATUS_DEVICE_DATA_ERROR
#define CR_TIMEOUT STATUS_IO_TIMEOUT
#define CR_BUSY STATUS_DEVICE_BUSY

/* CR_ERROR: true if NTSTATUS indicates failure */
#define CR_ERROR(x) (!NT_SUCCESS(x))

#else
#include <Uefi.h>
#include <Library/BaseLib.h>

typedef EFI_STATUS CR_STATUS;
#define CR_SUCCESS EFI_SUCCESS
#define CR_ABORT EFI_ABORTED
#define CR_INVALID_PARAMETER EFI_INVALID_PARAMETER
#define CR_NOT_FOUND EFI_NOT_FOUND
#define CR_OUT_OF_RESOURCES EFI_OUT_OF_RESOURCES
#define CR_UNSUPPORTED EFI_UNSUPPORTED
#define CR_DEVICE_ERROR EFI_DEVICE_ERROR
#define CR_TIMEOUT EFI_TIMEOUT
#define CR_BUSY EFI_ACCESS_DENIED
#define CR_ERROR(x) EFI_ERROR(x)
#endif // _KERNEL_MODE