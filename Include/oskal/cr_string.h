/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once
#include <oskal/cr_types.h>

#ifdef _KERNEL_MODE
#include <ntddk.h>
#include <ntstrsafe.h>

// memcmp wrapper
STATIC inline INTN
cr_memcmp(const VOID *dest, const VOID *src, UINTN len)
{
  SIZE_T matched = RtlCompareMemory(dest, src, len);
  if (matched == len) {
    return 0;
  }
  // Return difference at first mismatched byte
  return ((const UINT8 *)dest)[matched] - ((const UINT8 *)src)[matched];
}

// memcpy wrapper
STATIC inline VOID *
cr_memcpy(VOID *dest, VOID *src, UINTN len)
{
  RtlCopyMemory(dest, src, len);
  return dest;
}

// strncmp wrapper
STATIC inline INTN
cr_strncmp(const CHAR8 *str1, const CHAR8 *str2, UINTN len)
{
  for (UINTN i = 0; i < len; i++) {
    if (str1[i] != str2[i]) {
      return (UINT8)str1[i] - (UINT8)str2[i];
    }
    if (str1[i] == '\0') {
      return 0;
    }
  }
  return 0;
}

// strcmp wrapper
STATIC inline INTN
cr_strcmp(const CHAR8 *str1, const CHAR8 *str2)
{
  while (*str1 && (*str1 == *str2)) {
    str1++;
    str2++;
  }
  return (UINT8)*str1 - (UINT8)*str2;
}

#else
#include <Library/BaseMemoryLib.h>
// memcmp wrapper
STATIC inline INTN
cr_memcmp(const VOID *dest, const VOID *src, UINTN len)
{
  return CompareMem(dest, src, len);
}

// memcpy wrapper
STATIC inline VOID *
cr_memcpy(VOID *dest, VOID *src, UINTN len)
{
  return CopyMem(dest, src, len);
}

// strcmp_s wrapper
STATIC inline INTN
cr_strncmp(const CHAR8 *str1, const CHAR8 *str2, UINTN len)
{
  return AsciiStrnCmp(str1, str2, len);
}

// strcpm wrapper
STATIC inline INTN
cr_strcmp(const CHAR8 *str1, const CHAR8 *str2)
{
  return AsciiStrCmp(str1, str2);
}
#endif