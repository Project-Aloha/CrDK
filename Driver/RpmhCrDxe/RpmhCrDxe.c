/** @file
 *   Copyright (c) 2025-2026. Project Aloha Authors. All rights reserved.
 *   Copyright (c) 2025-2026. Kancy Joe. All rights reserved.
 *   SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <oskal/cr_debug.h>

#include <Library/rpmh.h>

#include <Protocol/EFICmdDBCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>

STATIC RpmhDeviceContext   *mRpmhContext   = NULL;
STATIC EFI_CMD_DB_PROTOCOL *mCmdDbProtocol = NULL;

EFI_STATUS
EFIAPI
ProtocolRpmhWrite(
    IN EFI_RPMH_CR_PROTOCOL *This, IN RpmhTcsCmd *TcsCmd, IN UINT32 NumCmds)
{
  EFI_STATUS Status;
  if (mRpmhContext == NULL) {
    log_err("ProtocolRpmhWrite: Failed to get Rpmh Context");
    return EFI_NOT_FOUND;
  }
  Status = RpmhWrite(mRpmhContext, TcsCmd, NumCmds);
  if (CR_ERROR(Status)) {
    log_err("ProtocolRpmhWrite: RpmhWrite failed with status 0x%X", Status);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ProtocolRpmhEnableVreg(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name, IN BOOLEAN Enable)
{
  EFI_STATUS Status;
  UINT32     Address = 0;

  if (mRpmhContext == NULL || mCmdDbProtocol == NULL || Name == NULL) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid Parameters", __FUNCTION__);
    return EFI_NOT_FOUND;
  }

  // Get vreg address from cmd db
  Status =
      mCmdDbProtocol->GetEntryAddressByName(mCmdDbProtocol, Name, &Address);
  if (EFI_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": GetCmdDBEntryAddressByName failed for " CR_LOG_CHAR8_STR_FMT ", "
        "Status=0x%X",
        __FUNCTION__, Name, Status);
    return Status;
  }
  Status = RpmhEnableVreg(mRpmhContext, Address, Enable);
  if (CR_ERROR(Status)) {
    log_err(CR_LOG_CHAR8_STR_FMT ": RpmhEnableVreg failed with status 0x%X", __FUNCTION__, Status);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ProtocolRpmhSetVregVoltage(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name,
    IN CONST UINT32 VoltageMv)
{
  EFI_STATUS Status;
  UINT32     Address = 0;

  if (mRpmhContext == NULL || mCmdDbProtocol == NULL || Name == NULL) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid Parameters", __FUNCTION__);
    return EFI_NOT_FOUND;
  }
  // Get vreg address from cmd db
  Status =
      mCmdDbProtocol->GetEntryAddressByName(mCmdDbProtocol, Name, &Address);
  if (EFI_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": GetCmdDBEntryAddressByName failed for " CR_LOG_CHAR8_STR_FMT ", "
        "Status=0x%X",
        __FUNCTION__, Name, Status);
    return Status;
  }

  Status = RpmhSetVregVoltage(mRpmhContext, Address, VoltageMv);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": RpmhSetVregVoltage failed with status "
        "0x%X",
        __FUNCTION__, Status);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ProtocolRpmhSetVregMode(
    IN EFI_RPMH_CR_PROTOCOL *This, IN CONST CHAR8 *Name, IN CONST UINT8 Mode)
{
  EFI_STATUS Status;
  UINT32     Address = 0;

  if (mRpmhContext == NULL || mCmdDbProtocol == NULL || Name == NULL) {
    log_err(CR_LOG_CHAR8_STR_FMT ": Invalid Parameters", __FUNCTION__);
    return EFI_NOT_FOUND;
  }

  // Get vreg address from cmd db
  Status =
      mCmdDbProtocol->GetEntryAddressByName(mCmdDbProtocol, Name, &Address);
  if (EFI_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": GetCmdDBEntryAddressByName failed for " CR_LOG_CHAR8_STR_FMT ", "
        "Status=0x%X",
        __FUNCTION__, Name, Status);
    return Status;
  }

  Status = RpmhSetVregMode(mRpmhContext, Address, Mode);
  if (CR_ERROR(Status)) {
    log_err(
        CR_LOG_CHAR8_STR_FMT ": RpmhSetVregMode failed with status 0x%X", __FUNCTION__, Status);
    return EFI_DEVICE_ERROR;
  }
  return EFI_SUCCESS;
}

EFI_RPMH_CR_PROTOCOL gRpmhCrProtocol = {
    .Revision           = EFI_RPMH_CR_PROTOCOL_REVISION,
    .RpmhWrite          = ProtocolRpmhWrite,
    .RpmhEnableVreg     = ProtocolRpmhEnableVreg,
    .RpmhSetVregVoltage = ProtocolRpmhSetVregVoltage,
    .RpmhSetVregMode    = ProtocolRpmhSetVregMode,
};

EFI_STATUS
RpmhEntryPoint(IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status = EFI_SUCCESS;

  // Locate Cmd DB protocol
  Status = gBS->LocateProtocol(
      &gEfiCmdDBCrProtocolGuid, NULL, (VOID **)&mCmdDbProtocol);
  if (EFI_ERROR(Status)) {
    log_err("Failed to locate Cmd DB Protocol, Status=0x%X", Status);
    return Status;
  }

  if (CR_ERROR(RpmhLibInit(&mRpmhContext)) || (mRpmhContext == NULL)) {
    log_err("RpmhLibInit failed");
    return EFI_DEVICE_ERROR;
  }

  // Install protocol
  Status = gBS->InstallMultipleProtocolInterfaces(
      &ImageHandle, &gEfiRpmhCrProtocolGuid, &gRpmhCrProtocol, NULL);
  if (EFI_ERROR(Status)) {
    log_err("Failed to install RPMH CR Protocol, Status=0x%X", Status);
    return Status;
  }

// test cases (for hdk sm8450)
#if 1
  {
    // Get vreg address from cmd db
    UINT32       Address   = 0;
    CONST CHAR8 *VregName  = "ldoc3"; // for hdk sm8450
    UINT32       VoltageMv = 3300;

    // Locate our protocol
    EFI_RPMH_CR_PROTOCOL *RpmhCrProtocol = NULL;
    Status                               = gBS->LocateProtocol(
        &gEfiRpmhCrProtocolGuid, NULL, (VOID **)&RpmhCrProtocol);
    if (EFI_ERROR(Status)) {
      log_err("Failed to locate Rpmh Cr Protocol, Status=0x%X", Status);
      return Status;
    }

    Status = mCmdDbProtocol->GetEntryAddressByName(
        mCmdDbProtocol, VregName, &Address);
    if (EFI_ERROR(Status)) {
      log_err(
          "GetCmdDBEntryAddressByName failed for " CR_LOG_CHAR8_STR_FMT ", Status=0x%X", VregName,
          Status);
      return Status;
    }

    // Print retrieved address
    log_info("Vreg " CR_LOG_CHAR8_STR_FMT " address: 0x%X", VregName, Address);

    // Enable vreg
    Status = RpmhCrProtocol->RpmhEnableVreg(RpmhCrProtocol, Address, TRUE);
    if (EFI_ERROR(Status)) {
      log_err("RpmhEnableVreg failed, Status=0x%X", Status);
      return Status;
    }
    log_info("Vreg " CR_LOG_CHAR8_STR_FMT " enabled", VregName);
    // Wait 5s for observation
    cr_sleep(5000);
    // Disable vreg
    Status = RpmhCrProtocol->RpmhEnableVreg(RpmhCrProtocol, Address, FALSE);
    if (EFI_ERROR(Status)) {
      log_err("RpmhEnableVreg failed, Status=0x%X", Status);
      return Status;
    }
    log_info("Vreg " CR_LOG_CHAR8_STR_FMT " disabled", VregName);
    // Wait 5s for observation
    cr_sleep(5000);

    // Set vreg voltage to 3.3v
    Status = RpmhCrProtocol->RpmhSetVregVoltage(
        RpmhCrProtocol, Address, VoltageMv); // in mV
    if (EFI_ERROR(Status)) {
      log_err("RpmhSetVregVoltage failed, Status=0x%X", Status);
      return Status;
    }
    log_info("Vreg " CR_LOG_CHAR8_STR_FMT " voltage set to %u mV", VregName, VoltageMv);
    // Wait 5s for observation
    cr_sleep(5000);
  }
#endif
  return EFI_SUCCESS;
}