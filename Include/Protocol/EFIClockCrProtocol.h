/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#define EFI_CLOCK_CR_PROTOCOL_REVISION 0x0000000000010000

#define EFI_CLOCK_CR_PROTOCOL_GUID                                             \
  {0xa355879b, 0x66c2, 0x4901, {0xb9, 0xa5, 0xc3, 0x0f, 0x31, 0x91, 0x81, 0xc8}}

extern EFI_GUID                       gEfiClockCrProtocolGuid;
typedef struct _EFI_CLOCK_CR_PROTOCOL EFI_CLOCK_CR_PROTOCOL;

typedef struct _EFI_CLOCK_CR_PROTOCOL {
  UINT64 Revision;
} EFI_CLOCK_CR_PROTOCOL;
