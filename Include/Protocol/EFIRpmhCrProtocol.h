/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#include <oskal/cr_types.h>
#include <Library/rpmh.h>

#define EFI_RPMH_CR_PROTOCOL_REVISION 0x0000000000010000

#define EFI_RPMH_CR_PROTOCOL_GUID                                              \
  {0x481e2514, 0xc4a9, 0x49a8, {0xaf, 0x78, 0x7a, 0x07, 0x77, 0xe9, 0xa0, 0x78}}

extern EFI_GUID                      gEfiRpmhCrProtocolGuid;
typedef struct _EFI_RPMH_CR_PROTOCOL EFI_RPMH_CR_PROTOCOL;

/* Note: All tcs cmds are sending to active onlt tcs currently */
typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_WRITE)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN RpmhTcsCmd *TcsCmd, IN UINT32 NumCmds);

typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_ENABLE_VREG)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name, IN BOOLEAN Enable);

typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_SET_VREG_VOLTAGE)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN CONST UINT32 VoltageMv);

typedef EFI_STATUS(EFIAPI *EFI_RPMH_CR_SET_VREG_MODE)(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN CONST UINT8 Mode);

typedef struct _EFI_RPMH_CR_PROTOCOL {
  UINT64                       Revision;
  EFI_RPMH_CR_WRITE            RpmhWrite;
  EFI_RPMH_CR_ENABLE_VREG      RpmhEnableVreg;
  EFI_RPMH_CR_SET_VREG_VOLTAGE RpmhSetVregVoltage;
  EFI_RPMH_CR_SET_VREG_MODE    RpmhSetVregMode;
} EFI_RPMH_CR_PROTOCOL;
