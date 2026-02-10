/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */
#pragma once

#include <oskal/common.h>
#include <oskal/cr_status.h>
#include <oskal/cr_types.h>
#include <oskal/cr_assert.h>

#define CMD_DB_MAX_SLAVES 8
#define CMD_DB_NUM_ENTRY_PRIORITIES 2
#define MAX_CMD_DB_AUX_DATA_LENGTH 40
#define SLAVE_ID_MSK GEN_MSK(18, 16)
#define RPMH_VRM_ADDRESS_MSK GEN_MSK(19, 4)

// Entries in cmddb resources
#pragma pack(push, 1)
typedef struct {
  UINT8  id[8];
  UINT32 priority[CMD_DB_NUM_ENTRY_PRIORITIES];
  UINT32 address;
  UINT16 length;
  UINT16 offset;
} EntryHeader;
CR_STATIC_ASSERT(sizeof(EntryHeader) == 24, "EntryHeader size incorrect");
#pragma pack(pop)

// Resources in cmddb header
#pragma pack(push, 1)
typedef struct {
  UINT16 slaveId;
  UINT16 headerOffset;
  UINT16 dataOffset;
  UINT16 count;
  UINT16 version;
  UINT16 reserved[3];
} ResourceHeader;
CR_STATIC_ASSERT(sizeof(ResourceHeader) == 16, "ResourceHeader size incorrect");
#pragma pack(pop)

// Start of cmd-db file
#pragma pack(push, 1)
typedef struct {
  UINT32         version;
  UINT32         magic;
  ResourceHeader rscHeaders[CMD_DB_MAX_SLAVES];
  UINT32         checksum;
  UINT32         reserved;
  // UINT8          data[ANYSIZE_ARRAY];
  CR_FLEXIBLE_ARRAY_MEMBER(UINT8, data);
} CmdDbHeader;
#pragma pack(pop)
CR_STATIC_ASSERT(sizeof(CmdDbHeader) == 144 + CR_FLEXIBLE_ARRAY_SIZE(UINT8), "CmdDbHeader size incorrect");

// Slave types
enum RSC_SLAVE_ID_TYPE {
  RSC_SLAVE_HW_INVALID = 0,
  RSC_SLAVE_HW_ARC     = 3,
  RSC_SLAVE_HW_VRM     = 4,
  RSC_SLAVE_HW_BCM     = 5,
  RSC_SLAVE_HW_MAX     = 0xFF
};

BOOLEAN ValidateCmdDBHeader(IN CmdDbHeader *cmd_db_header);

VOID DumpCmdDBInfo(IN CmdDbHeader *cmd_db_header);

CR_STATUS
GetCmdDBEntryAddressByName(
    IN CmdDbHeader *cmd_db_header, IN CONST CHAR8 *entry_name,
    OUT UINT32 *address);

CR_STATUS
GetCmdDBEntryNameByAddress(
    IN CmdDbHeader *cmd_db_header, IN UINT32 address, OUT CHAR8 *entry_name);

CR_STATUS
GetCmdDBAuxDataByName(
    IN CmdDbHeader *cmd_db_header, IN CONST CHAR8 *name, OUT UINT8 *aux_data,
    OUT UINT32 *length);

CR_STATUS
GetCmdDBAuxDataByAddress(
    IN CmdDbHeader *cmd_db_header, IN UINT32 address, OUT UINT8 *aux_data,
    OUT UINT32 *length);

STATIC inline BOOLEAN CmdDBIsAddrEqual(IN UINT32 Addr1, IN UINT32 Addr2)
{
  // Special workaround for VRM
  if ((Addr1 == Addr2) ||
      ((GET_FIELD(Addr1, SLAVE_ID_MSK) == RSC_SLAVE_HW_VRM) &&
          (GET_FIELD(Addr2, RPMH_VRM_ADDRESS_MSK) ==
           GET_FIELD(Addr1, RPMH_VRM_ADDRESS_MSK)))) {
    return TRUE;
  }

  return FALSE;
}