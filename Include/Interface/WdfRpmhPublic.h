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
// 6279008e-9a20-4ac7-87b9-0ef233f661a8
DEFINE_GUID(GUID_RPMH_CR_INTERFACE, 0x6279008e, 0x9a20, 0x4ac7, 0x87, 0xb9, 0x0e,
            0xf2, 0x33, 0xf6, 0x61, 0xa8);

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
