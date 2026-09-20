/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#include "spmi_internal.h"

UINTN
PmicArbGetOwnerAccStatusV3(SpmiDeviceContext *Ctx, UINT8 Ee, UINT16 Index) {
  return Ctx->Bus.InterruptAddr + 0x200000 + 0x1000 * Ee + 0x4 * Index;
}
