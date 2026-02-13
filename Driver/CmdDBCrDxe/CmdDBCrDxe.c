/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/cmddb.h>

#include <Protocol/EFICmdDBCrProtocol.h>

// Global pointer to CmdDB header
STATIC CmdDbHeader *gCmdDbHeader = NULL;

// Protocol wrapper functions
EFI_STATUS
EFIAPI
ProtocolGetEntryAddressByName(
    IN EFI_CMD_DB_PROTOCOL *This, IN CONST CHAR8 *Name, OUT UINT32 *Address)
{
  if (gCmdDbHeader == NULL || Name == NULL || Address == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CR_STATUS Status = GetCmdDBEntryAddressByName(gCmdDbHeader, Name, Address);
  return (Status == CR_SUCCESS) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
ProtocolGetEntryNameByAddress(
    IN EFI_CMD_DB_PROTOCOL *This, IN UINT32 Address, OUT CONST CHAR8 *Name)
{
  if (gCmdDbHeader == NULL || Name == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CR_STATUS Status =
      GetCmdDBEntryNameByAddress(gCmdDbHeader, (UINT32)Address, (CHAR8 *)Name);
  return (Status == CR_SUCCESS) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
ProtocolGetAuxDataByName(
    IN EFI_CMD_DB_PROTOCOL *This, IN CONST CHAR8 *Name, OUT UINT8 *AuxData,
    OUT UINT32 *Length)
{
  if (gCmdDbHeader == NULL || Name == NULL || AuxData == NULL ||
      Length == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CR_STATUS Status = GetCmdDBAuxDataByName(gCmdDbHeader, Name, AuxData, Length);
  return (Status == CR_SUCCESS) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_STATUS
EFIAPI
ProtocolGetAuxDataByAddress(
    IN EFI_CMD_DB_PROTOCOL *This, IN CONST UINT32 Address, OUT UINT8 *AuxData,
    OUT UINT32 *Length)
{
  if (gCmdDbHeader == NULL || AuxData == NULL || Length == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  CR_STATUS Status =
      GetCmdDBAuxDataByAddress(gCmdDbHeader, (UINT32)Address, AuxData, Length);
  return (Status == CR_SUCCESS) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

EFI_CMD_DB_PROTOCOL gCmdDBProtocol = {
    EFI_CMD_DB_PROTOCOL_REVISION, ProtocolGetEntryAddressByName,
    ProtocolGetEntryNameByAddress, ProtocolGetAuxDataByName,
    ProtocolGetAuxDataByAddress};

EFI_STATUS
CmdDBEntryPoint(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // Get AOP CMD DB address from memory map
  ARM_MEMORY_REGION_DESCRIPTOR_EX CmdDBMemoryRegion = {0};
  Status = LocateMemoryMapAreaByName("AOP CMD DB", &CmdDBMemoryRegion);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (EFI_D_ERROR,
         "CmdDBEntryPoint: LocateMemoryMapAreaByName returned %r\n", Status));
    return Status;
  }

  // Validate AOP CMD DB region
  gCmdDbHeader = (CmdDbHeader *)(UINTN)CmdDBMemoryRegion.Address;
  if (!ValidateCmdDBHeader(gCmdDbHeader)) {
    DEBUG((EFI_D_ERROR, "CmdDBEntryPoint: Invalid CmdDB Header\n"));
    gCmdDbHeader = NULL;
    return EFI_ABORTED;
  }

  // Install protocol
  Status = gBS->InstallMultipleProtocolInterfaces(
      &ImageHandle, &gEfiCmdDBCrProtocolGuid, &gCmdDBProtocol, NULL, NULL);
  if (EFI_ERROR(Status)) {
    DEBUG(
        (EFI_D_ERROR, "CmdDBEntryPoint: InstallProtocolInterface returned %r\n",
         Status));
    return Status;
  }

// Testcases
#if 0
  {
    UINT32 entry_address = 0;
    // Dump CmdDB information to see all available entries
    DumpCmdDBInfo(gCmdDbHeader);

    EFI_CMD_DB_PROTOCOL *CmdDBProtocol = NULL;
    Status                             = gBS->LocateProtocol(
        &gEfiCmdDBCrProtocolGuid, NULL, (VOID **)&CmdDBProtocol);
    if (EFI_ERROR(Status)) {
      DEBUG(
          (EFI_D_ERROR, "CmdDBEntryPoint: LocateProtocol returned %r\n",
           Status));
      return Status;
    }

    Status = CmdDBProtocol->GetEntryAddressByName(
        CmdDBProtocol, "ebi.lvl", &entry_address);
    DEBUG(
        (EFI_D_WARN, "CmdDBEntryPoint: Protocol ebi.lvl address = 0x%08X\n",
         entry_address));
    Status = CmdDBProtocol->GetEntryAddressByName(
        CmdDBProtocol, "lmx.mol", &entry_address);
    DEBUG(
        (EFI_D_WARN, "CmdDBEntryPoint: Protocol lmx.mol address = 0x%08X\n",
         entry_address));
    Status = CmdDBProtocol->GetEntryAddressByName(
        CmdDBProtocol, "CE0", &entry_address);
    DEBUG(
        (EFI_D_WARN, "CmdDBEntryPoint: Protocol CE0 address = 0x%08X\n",
         entry_address));
    Status = CmdDBProtocol->GetEntryAddressByName(
        CmdDBProtocol, "rfclka1", &entry_address);
    DEBUG(
        (EFI_D_WARN, "CmdDBEntryPoint: Protocol rfclka1 address = 0x%08X\n",
         entry_address));

    UINT8  aux_data[MAX_CMD_DB_AUX_DATA_LENGTH] = {0};
    UINT32 length                               = 0;
    Status = CmdDBProtocol->GetAuxDataByName(
        CmdDBProtocol, "rfclka1", aux_data, &length);
    DEBUG(
        (EFI_D_WARN,
         "CmdDBEntryPoint: Protocol rfclka1 aux data length = %d\nData: [ ",
         length));
    for (UINTN i = 0; i < length; i++) {
      DEBUG((EFI_D_WARN, "%02X ", aux_data[i]));
    }
    DEBUG((EFI_D_WARN, "]\n"));

    // Get entry name by address
    CHAR8 entry_name[16] = {0};
    Status               = CmdDBProtocol->GetEntryNameByAddress(
        CmdDBProtocol, 0x00030020, entry_name);
    DEBUG(
        (EFI_D_WARN,
         "CmdDBEntryPoint: Protocol entry name for address 0x00030020 "
         "= " CR_LOG_CHAR8_STR_FMT "\n",
         entry_name));
    Status = CmdDBProtocol->GetEntryNameByAddress(
        CmdDBProtocol, 0x0005003C, entry_name);
    DEBUG(
        (EFI_D_WARN,
         "CmdDBEntryPoint: Protocol entry name for address 0x0005003C "
         "= " CR_LOG_CHAR8_STR_FMT "\n",
         entry_name));
    Status = CmdDBProtocol->GetEntryNameByAddress(
        CmdDBProtocol, 0x00045000, entry_name);
    DEBUG(
        (EFI_D_WARN,
         "CmdDBEntryPoint: Protocol entry name for address 0x00045000 "
         "= " CR_LOG_CHAR8_STR_FMT "\n",
         entry_name));

    // Get aux data by address
    Status = CmdDBProtocol->GetAuxDataByAddress(
        CmdDBProtocol, 0x00030020, aux_data, &length);
    DEBUG(
        (EFI_D_WARN,
         "CmdDBEntryPoint: Protocol aux data for address 0x00030020 length = "
         "%d\nData: [ ",
         length));
    for (UINTN i = 0; i < length; i++) {
      DEBUG((EFI_D_WARN, "%02X ", aux_data[i]));
    }
    DEBUG((EFI_D_WARN, "]\n"));
    Status = CmdDBProtocol->GetAuxDataByAddress(
        CmdDBProtocol, 0x0005003C, aux_data, &length);
    DEBUG(
        (EFI_D_WARN,
         "CmdDBEntryPoint: Protocol aux data for address 0x0005003C length = "
         "%d\nData: [ ",
         length));
    for (UINTN i = 0; i < length; i++) {
      DEBUG((EFI_D_WARN, "%02X ", aux_data[i]));
    }
    DEBUG((EFI_D_WARN, "]\n"));
  }
#endif

  return EFI_SUCCESS;
}