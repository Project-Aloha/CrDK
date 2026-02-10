/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#pragma once

#define EFI_CMD_DB_PROTOCOL_REVISION 0x0000000000010000

#define EFI_CMD_DB_PROTOCOL_GUID                                               \
  {0x5b0dbbb7, 0x88b2, 0x8326, {0xba, 0x22, 0x33, 0xff, 0xde, 0x01, 0x08, 0x6f}}

extern EFI_GUID                     gEfiCmdDBCrProtocolGuid;
typedef struct _EFI_CMD_DB_PROTOCOL EFI_CMD_DB_PROTOCOL;

typedef EFI_STATUS(EFIAPI *EFI_CMD_DB_GET_ADDR_BY_NAME)(
    IN EFI_CMD_DB_PROTOCOL *This, IN CONST CHAR8 *Name, OUT UINT32 *Address);

typedef EFI_STATUS(EFIAPI *EFI_CMD_DB_GET_NAME_BY_ADDR)(
    IN EFI_CMD_DB_PROTOCOL *This, IN UINT32 Address, OUT CONST CHAR8 *Name);

typedef EFI_STATUS(EFIAPI *EFI_CMD_DB_GET_AUX_DATA_BY_NAME)(
    IN EFI_CMD_DB_PROTOCOL *This, IN CONST CHAR8 *Name, OUT UINT8 *AuxData,
    OUT UINT32 *Length);

typedef EFI_STATUS(EFIAPI *EFI_CMD_DB_GET_AUX_DATA_BY_ADDR)(
    IN EFI_CMD_DB_PROTOCOL *This, IN CONST UINT32 Address, OUT UINT8 *AuxData,
    OUT UINT32 *Length);

typedef struct _EFI_CMD_DB_PROTOCOL {
  UINT64                          Revision;
  EFI_CMD_DB_GET_ADDR_BY_NAME     GetEntryAddressByName;
  EFI_CMD_DB_GET_NAME_BY_ADDR     GetEntryNameByAddress;
  EFI_CMD_DB_GET_AUX_DATA_BY_NAME GetAuxDataByName;
  EFI_CMD_DB_GET_AUX_DATA_BY_ADDR GetAuxDataByAddress;
} EFI_CMD_DB_PROTOCOL;