/** @file
 *  EFI owner for Qualcomm RPMh ICB bandwidth votes.
 *
 *  CmdDB and RPMh remain behind their existing protocols.  PCIe consumers
 *  receive only path handles and cannot issue raw BCM commands themselves.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/interconnect.h>

#include <Protocol/EFICmdDBCrProtocol.h>
#include <Protocol/EFIInterconnectCrProtocol.h>
#include <Protocol/EFIRpmhCrProtocol.h>

STATIC EFI_CMD_DB_PROTOCOL *mCmdDb;
STATIC EFI_RPMH_CR_PROTOCOL *mRpmh;
STATIC InterconnectDeviceContext *mInterconnect;

STATIC CR_STATUS
InterconnectGetAddress(
    IN VOID *Context, IN CONST CHAR8 *Name, OUT UINT32 *Address)
{
  EFI_CMD_DB_PROTOCOL *Protocol = (EFI_CMD_DB_PROTOCOL *)Context;

  if (Protocol == NULL || Protocol->GetEntryAddressByName == NULL ||
      Name == NULL || Address == NULL) {
    return CR_INVALID_PARAMETER;
  }
  return EFI_ERROR(Protocol->GetEntryAddressByName(Protocol, Name, Address))
             ? CR_NOT_FOUND
             : CR_SUCCESS;
}

STATIC CR_STATUS
InterconnectGetAuxData(
    IN VOID *Context, IN CONST CHAR8 *Name, OUT UINT8 *Data,
    IN OUT UINT32 *Length)
{
  EFI_CMD_DB_PROTOCOL *Protocol = (EFI_CMD_DB_PROTOCOL *)Context;
  EFI_STATUS Status;

  if (Protocol == NULL || Protocol->GetAuxDataByName == NULL ||
      Name == NULL || Data == NULL || Length == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Status = Protocol->GetAuxDataByName(Protocol, Name, Data, Length);
  return EFI_ERROR(Status) ? CR_NOT_FOUND : CR_SUCCESS;
}

STATIC CR_STATUS
InterconnectWriteRpmh(
    IN VOID *Context, IN RpmhTcsCmd *Commands, IN UINT32 CommandCount)
{
  EFI_RPMH_CR_PROTOCOL *Protocol = (EFI_RPMH_CR_PROTOCOL *)Context;

  if (Protocol == NULL || Protocol->RpmhWrite == NULL || Commands == NULL ||
      CommandCount == 0) {
    return CR_INVALID_PARAMETER;
  }
  return EFI_ERROR(Protocol->RpmhWrite(Protocol, Commands, CommandCount))
             ? CR_DEVICE_ERROR
             : CR_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
ProtocolAcquirePath(
    IN EFI_INTERCONNECT_CR_PROTOCOL *This,
    IN CONST CHAR8                  *ProviderCompatible,
    IN UINT32                        SourceId,
    IN UINT32                        DestinationId,
    OUT INTERCONNECT_PATH_HANDLE    *Path)
{
  CR_STATUS Status;

  (VOID)This;
  if (mInterconnect == NULL || ProviderCompatible == NULL || Path == NULL) {
    return EFI_NOT_READY;
  }
  Status = InterconnectAcquirePath(
      mInterconnect, ProviderCompatible, SourceId, DestinationId, Path);
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
ProtocolSetBandwidth(
    IN EFI_INTERCONNECT_CR_PROTOCOL *This,
    IN INTERCONNECT_PATH_HANDLE     Path,
    IN UINT64                        AverageBandwidth,
    IN UINT64                        PeakBandwidth)
{
  CR_STATUS Status;

  (VOID)This;
  if (mInterconnect == NULL) {
    return EFI_NOT_READY;
  }
  Status = InterconnectSetBandwidth(
      mInterconnect, Path, AverageBandwidth, PeakBandwidth);
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_STATUS
EFIAPI
ProtocolReleasePath(
    IN EFI_INTERCONNECT_CR_PROTOCOL *This,
    IN INTERCONNECT_PATH_HANDLE     Path)
{
  CR_STATUS Status;

  (VOID)This;
  if (mInterconnect == NULL) {
    return EFI_NOT_READY;
  }
  Status = InterconnectReleasePath(mInterconnect, Path);
  return CR_ERROR(Status) ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}

STATIC EFI_INTERCONNECT_CR_PROTOCOL mInterconnectProtocol = {
    .Revision     = EFI_INTERCONNECT_CR_PROTOCOL_REVISION,
    .AcquirePath = ProtocolAcquirePath,
    .SetBandwidth = ProtocolSetBandwidth,
    .ReleasePath = ProtocolReleasePath,
};

EFI_STATUS
EFIAPI
InterconnectEntryPoint(
    IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  EFI_STATUS Status;
  InterconnectIoOps Io;

  (VOID)SystemTable;
  Status = gBS->LocateProtocol(
      &gEfiCmdDBCrProtocolGuid, NULL, (VOID **)&mCmdDb);
  if (EFI_ERROR(Status)) {
    return Status;
  }
  Status = gBS->LocateProtocol(
      &gEfiRpmhCrProtocolGuid, NULL, (VOID **)&mRpmh);
  if (EFI_ERROR(Status)) {
    return Status;
  }

  ZeroMem(&Io, sizeof(Io));
  Io.GetAddress = InterconnectGetAddress;
  Io.GetAuxData = InterconnectGetAuxData;
  Io.WriteRpmh = InterconnectWriteRpmh;
  Io.CmdDbContext = mCmdDb;
  Io.RpmhContext = mRpmh;
  Io.MaxRpmhCommands = INTERCONNECT_MAX_RPMH_COMMANDS;
  if (CR_ERROR(InterconnectLibInit(&mInterconnect, &Io)) ||
      mInterconnect == NULL) {
    DEBUG((DEBUG_ERROR, "ICBCrDxe: target initialization failed\n"));
    return EFI_DEVICE_ERROR;
  }

  return gBS->InstallMultipleProtocolInterfaces(
      &ImageHandle, &gEfiInterconnectCrProtocolGuid,
      &mInterconnectProtocol, NULL);
}
