/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <wdm.h>
#include <oskal/cr_types.h>

#define WDF_CMD_DB_INTERFACE_REVISION 0x1U

DEFINE_GUID(GUID_DEVINTERFACE_CMDDB_CR, 0xfed72545, 0x00c8, 0x46a5, 0x97, 0x75,
            0xbd, 0xb5, 0x34, 0x80, 0x00, 0x12);

DEFINE_GUID(GUID_CMDDB_INTERFACE, 0x6279008e, 0x9a20, 0x4ac7, 0x87, 0xb9, 0x0e,
            0xf2, 0x33, 0xf6, 0x61, 0xa8);

typedef NTSTATUS (*CMD_DB_GET_ADDR_BY_NAME)(IN VOID *Context,
                                            IN CONST CHAR8 *Name,
                                            OUT UINT32 *Address);

typedef NTSTATUS (*CMD_DB_GET_NAME_BY_ADDR)(IN VOID *Context, IN UINT32 Address,
                                            OUT CHAR8 *Name);

typedef NTSTATUS (*CMD_DB_GET_AUX_DATA_BY_NAME)(IN VOID *Context,
                                                IN CONST CHAR8 *Name,
                                                OUT UINT8 *AuxData,
                                                OUT UINT32 *Length);

typedef NTSTATUS (*CMD_DB_GET_AUX_DATA_BY_ADDR)(IN VOID *Context,
                                                IN CONST UINT32 Address,
                                                OUT UINT8 *AuxData,
                                                OUT UINT32 *Length);

typedef struct _WDF_CMD_DB_INTERFACE {
  INTERFACE Header;
  CMD_DB_GET_ADDR_BY_NAME GetEntryAddressByName;
  CMD_DB_GET_NAME_BY_ADDR GetEntryNameByAddress;
  CMD_DB_GET_AUX_DATA_BY_NAME GetAuxDataByName;
  CMD_DB_GET_AUX_DATA_BY_ADDR GetAuxDataByAddress;
} WDF_CMD_DB_INTERFACE;