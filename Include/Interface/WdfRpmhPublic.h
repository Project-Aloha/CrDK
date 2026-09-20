/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <wdm.h>
#include <oskal/cr_types.h>
#include <Library/rpmh.h>

#define WDF_RPMH_CR_INTERFACE_REVISION 0x1U
// 7e735ed4-d6b6-44a7-9c55-075e4e0c9a4e
DEFINE_GUID(GUID_DEVINTERFACE_RPMH_CR, 0x7e735ed4, 0xd6b6, 0x44a7, 0x9c, 0x55,
            0x07, 0x5e, 0x4e, 0x0c, 0x9a, 0x4e);
// 8fcbb757-66b2-4d58-9676-ae2a4beff2aa
DEFINE_GUID(GUID_RPMH_CR_INTERFACE, 0x8fcbb757, 0x66b2, 0x4d58, 0x96, 0x76, 0xae,
            0x2a, 0x4b, 0xef, 0xf2, 0xaa);

/* Note: All tcs cmds are sending to active onlt tcs currently */
typedef NTSTATUS(*RPMH_CR_WRITE)(IN VOID *Context, IN RpmhTcsCmd *TcsCmd,
                                IN UINT32 NumCmds);

typedef NTSTATUS(*RPMH_CR_ENABLE_VREG)(IN VOID *Context, IN CONST CHAR8 *Name,
                                      IN BOOLEAN Enable);

typedef NTSTATUS(*RPMH_CR_SET_VREG_VOLTAGE)(IN VOID *Context,
                                           IN CONST CHAR8 *Name,
                                           IN CONST UINT32 VoltageMv);

typedef NTSTATUS(*RPMH_CR_SET_VREG_MODE)(IN VOID *Context, IN CONST CHAR8 *Name,
                                        IN CONST UINT8 Mode);

typedef struct _WDF_RPMH_CR_INTERFACE {
  INTERFACE Header;
  RPMH_CR_WRITE RpmhWrite;
  RPMH_CR_ENABLE_VREG RpmhEnableVreg;
  RPMH_CR_SET_VREG_VOLTAGE RpmhSetVregVoltage;
  RPMH_CR_SET_VREG_MODE RpmhSetVregMode;
} WDF_RPMH_CR_INTERFACE;
