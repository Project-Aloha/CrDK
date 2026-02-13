/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include "cmddb_internal.h"
// For WPP tracing
#ifdef _KERNEL_MODE
#include "cmddb.tmh"
#endif

// Validate CmdDB magic
BOOLEAN
ValidateCmdDBHeader(IN CmdDbHeader *cmd_db_header)
{
  if (cmd_db_header->magic == CMD_DB_MAGIC)
    return TRUE;
  log_err("Invalid cmd-db magic 0x%08X", cmd_db_header->magic);
  return FALSE;
}

// Seach cmd db region for request entry
CR_STATUS
FindCmdDBEntry(
    IN CmdDbHeader *cmd_db_header, IN UINT32 address, IN CONST CHAR8 *name,
    OUT cmd_db_entry *entry)
{
  if (!cmd_db_header || !entry)
    return CR_INVALID_PARAMETER;

  if (!address && !name) {
    log_warn("Ignore searching entry with (address == 0)");
    return CR_INVALID_PARAMETER;
  }

  for (UINTN i = 0; i < CMD_DB_MAX_SLAVES; i++) {
    // If address was provided, skip non-matching slave IDs
    if (cmd_db_header->rscHeaders[i].slaveId == RSC_SLAVE_HW_INVALID ||
        (address && (cmd_db_header->rscHeaders[i].slaveId != GET_FIELD(
                         address, SLAVE_ID_MSK))))
      continue;
    ResourceHeader *rsc_header = &cmd_db_header->rscHeaders[i];
    for (UINTN j = 0; j < rsc_header->count; j++) {
      EntryHeader *tmp_entry_hdr =
          (EntryHeader *)&cmd_db_header
              ->data[rsc_header->headerOffset + j * sizeof(EntryHeader)];
      if ((address && (tmp_entry_hdr->address == address)) ||
          (name && cr_strncmp(
                       (const CHAR8 *)tmp_entry_hdr->id, name,
                       sizeof(tmp_entry_hdr->id)) == 0)) {
        entry->Address = tmp_entry_hdr->address;
        cr_memcpy(entry->Name, tmp_entry_hdr->id, sizeof(entry->Name));
        entry->DataLength = tmp_entry_hdr->length;
        entry->Data =
            &cmd_db_header
                 ->data[rsc_header->dataOffset + tmp_entry_hdr->offset];
        return CR_SUCCESS;
      }
    }
  }
  return CR_NOT_FOUND;
}

// Dump CmdDB Info
VOID DumpCmdDBInfo(IN CmdDbHeader *cmd_db_header)
{
  if (!cmd_db_header || !ValidateCmdDBHeader(cmd_db_header)) {
    log_err("Invalid CmdDB Header");
    return;
  }
  // Print CmdDB Header & Rsc Header Info
  log_warn("==================================");
  log_warn("       Header Information         ");
  log_warn("==================================");
  log_info("Version: %u", cmd_db_header->version);
  log_info("Resources:");
  for (UINTN i = 0; i < CMD_DB_MAX_SLAVES; i++) {
    ResourceHeader *rsc_header = &cmd_db_header->rscHeaders[i];
    if (rsc_header->slaveId == RSC_SLAVE_HW_INVALID)
      continue;
    log_info("  Slave ID: %u", rsc_header->slaveId);
    log_info(
        "    Slave Type: " CR_LOG_CHAR8_STR_FMT,
        rsc_slave_id_to_string((enum RSC_SLAVE_ID_TYPE)rsc_header->slaveId));
    log_info("    Entries Count: %u", rsc_header->count);
    log_info(
        "    Version: %d.%d", rsc_header->version >> 8,
        rsc_header->version & 0xFF);
  }

  // Parse and print entries in each resources
  log_warn("==================================");
  log_warn("        Resource Entries         ");
  log_warn("==================================");
  for (UINTN i = 0; i < CMD_DB_MAX_SLAVES; i++) {
    ResourceHeader *rsc_header = &cmd_db_header->rscHeaders[i];
    if (rsc_header->slaveId == RSC_SLAVE_HW_INVALID)
      continue;
    log_warn("----------------------------------");
    log_warn(
        "Entries for Slave ID: " CR_LOG_CHAR8_STR_FMT,
        rsc_slave_id_to_string((enum RSC_SLAVE_ID_TYPE)rsc_header->slaveId));
    log_warn("----------------------------------");
    // Get offset to entries
    for (UINTN j = 0; j < rsc_header->count; j++) {
      EntryHeader *tmp_entry_hdr =
          (EntryHeader *)&cmd_db_header
              ->data[rsc_header->headerOffset + j * sizeof(EntryHeader)];
      // Print entry info
      log_info("Entry ID: %c%c%c%c%c%c%c%c",
          tmp_entry_hdr->id[0], tmp_entry_hdr->id[1],
          tmp_entry_hdr->id[2], tmp_entry_hdr->id[3],
          tmp_entry_hdr->id[4], tmp_entry_hdr->id[5],
          tmp_entry_hdr->id[6], tmp_entry_hdr->id[7]
      );
      log_info("  Address: 0x%08X", tmp_entry_hdr->address);
      log_info("  Length: %u", tmp_entry_hdr->length);
      log_info("  Offset: %u", tmp_entry_hdr->offset);
      // Print command data in hex
      if (tmp_entry_hdr->length == 0)
        continue;
      log_info("  Command Data:");
      log_raw(LOG_COLOR_INFO "[INFO]   ");
      for (UINTN k = 0; k < tmp_entry_hdr->length; k++) {
        log_raw(
            LOG_COLOR_INFO " %02X",
            cmd_db_header
                ->data[rsc_header->dataOffset + tmp_entry_hdr->offset + k]);
      }
      log_raw("\n");
    }
  }
}

// Get address by name
CR_STATUS
GetCmdDBEntryAddressByName(
    IN CmdDbHeader *cmd_db_header, IN CONST CHAR8 *entry_name,
    OUT UINT32 *address)
{
  if (!cmd_db_header || !entry_name || !address)
    return CR_INVALID_PARAMETER;

  cmd_db_entry entry  = {0};
  CR_STATUS    status = FindCmdDBEntry(cmd_db_header, 0, entry_name, &entry);
  if (status != CR_SUCCESS)
    return status;

  *address = entry.Address;
  return CR_SUCCESS;
}

// Get name by address
CR_STATUS
GetCmdDBEntryNameByAddress(
    IN CmdDbHeader *cmd_db_header, IN UINT32 address, IN OUT CHAR8 *entry_name)
{
  if (!cmd_db_header || !entry_name)
    return CR_INVALID_PARAMETER;

  cmd_db_entry entry  = {0};
  CR_STATUS    status = FindCmdDBEntry(cmd_db_header, address, NULL, &entry);
  if (status != CR_SUCCESS)
    return status;

  cr_memcpy(entry_name, entry.Name, sizeof(entry.Name));
  return CR_SUCCESS;
}

// Get aux data
CR_STATUS
GetCmdDBAuxDataByName(
    IN CmdDbHeader *cmd_db_header, IN CONST CHAR8 *name, OUT UINT8 *aux_data,
    OUT UINT32 *length)
{
  if (!cmd_db_header || !aux_data)
    return CR_INVALID_PARAMETER;

  cmd_db_entry entry  = {0};
  CR_STATUS    status = FindCmdDBEntry(cmd_db_header, 0, name, &entry);
  if (status != CR_SUCCESS)
    return status;

  *length = entry.DataLength;
  cr_memcpy(aux_data, entry.Data, entry.DataLength);
  return CR_SUCCESS;
}

CR_STATUS
GetCmdDBAuxDataByAddress(
    IN CmdDbHeader *cmd_db_header, IN UINT32 address, OUT UINT8 *aux_data,
    OUT UINT32 *length)
{
  if (!cmd_db_header || !aux_data || address == 0)
    return CR_INVALID_PARAMETER;

  cmd_db_entry entry  = {0};
  CR_STATUS    status = FindCmdDBEntry(cmd_db_header, address, NULL, &entry);
  if (status != CR_SUCCESS)
    return status;

  *length = entry.DataLength;
  cr_memcpy(aux_data, entry.Data, entry.DataLength);
  return CR_SUCCESS;
}