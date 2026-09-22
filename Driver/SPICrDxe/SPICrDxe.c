/** @file
 *  Alias the standard MU SPI host-controller protocol for Crane consumers.
 *
 *  The MU SPIDxe binary remains the only GENI/SPI hardware owner.  This
 *  adapter forwards the standard EFI_SPI_HC_PROTOCOL on a separate handle and
 *  fails closed when no standard MU protocol is present.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/EFIGpiCrProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>
#include <Protocol/EFISpiCrProtocol.h>

typedef struct {
  EFI_HANDLE       AliasHandle;
  EFI_SPI_CR_PROTOCOL Protocol;
} SPI_CR_INSTANCE;

STATIC SPI_CR_INSTANCE *mInstances;
STATIC UINTN            mInstanceCount;

STATIC SPI_CR_INSTANCE *
SpiCrInstanceFromProtocol (
  IN EFI_SPI_CR_PROTOCOL *This
  )
{
  if (This == NULL) {
    return NULL;
  }
  return BASE_CR (This, SPI_CR_INSTANCE, Protocol);
}

STATIC EFI_STATUS
EFIAPI
SpiCrChipSelect (
  IN EFI_SPI_CR_PROTOCOL      *This,
  IN CONST EFI_SPI_PERIPHERAL *SpiPeripheral,
  IN BOOLEAN                   PinValue
  )
{
  SPI_CR_INSTANCE *Instance;

  Instance = SpiCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Host == NULL) ||
      (Instance->Protocol.Host->ChipSelect == NULL)) {
    return EFI_NOT_READY;
  }
  return Instance->Protocol.Host->ChipSelect (
      Instance->Protocol.Host, SpiPeripheral, PinValue);
}

STATIC EFI_STATUS
EFIAPI
SpiCrClock (
  IN EFI_SPI_CR_PROTOCOL      *This,
  IN CONST EFI_SPI_PERIPHERAL *SpiPeripheral,
  IN UINT32                   *ClockHz
  )
{
  SPI_CR_INSTANCE *Instance;

  Instance = SpiCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Host == NULL) ||
      (Instance->Protocol.Host->Clock == NULL)) {
    return EFI_NOT_READY;
  }
  return Instance->Protocol.Host->Clock (
      Instance->Protocol.Host, SpiPeripheral, ClockHz);
}

STATIC EFI_STATUS
EFIAPI
SpiCrTransaction (
  IN EFI_SPI_CR_PROTOCOL    *This,
  IN EFI_SPI_BUS_TRANSACTION *BusTransaction
  )
{
  SPI_CR_INSTANCE *Instance;

  Instance = SpiCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Host == NULL) ||
      (Instance->Protocol.Host->Transaction == NULL)) {
    return EFI_NOT_READY;
  }
  return Instance->Protocol.Host->Transaction (
      Instance->Protocol.Host, BusTransaction);
}

EFI_STATUS
EFIAPI
SpiCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS              Status;
  EFI_HANDLE             *Handles;
  UINTN                   HandleCount;
  UINTN                   Index;
  UINTN                   Installed;
  EFI_SPI_HC_PROTOCOL    *Host;

  (VOID)SystemTable;
  Handles = NULL;
  HandleCount = 0;
  Installed = 0;

  Status = gBS->LocateHandleBuffer (
      ByProtocol, &gEfiSpiHcProtocolGuid, NULL,
      &HandleCount, &Handles);
  if (EFI_ERROR (Status) || (HandleCount == 0) || (Handles == NULL)) {
    return EFI_NOT_FOUND;
  }

  mInstances = AllocateZeroPool (HandleCount * sizeof (*mInstances));
  if (mInstances == NULL) {
    FreePool (Handles);
    return EFI_OUT_OF_RESOURCES;
  }

  for (Index = 0; Index < HandleCount; ++Index) {
    EFI_HANDLE AliasHandle;

    Host = NULL;
    Status = gBS->OpenProtocol (
        Handles[Index], &gEfiSpiHcProtocolGuid, (VOID **)&Host,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR (Status) || (Host == NULL) ||
        (Host->ChipSelect == NULL) || (Host->Clock == NULL) ||
        (Host->Transaction == NULL)) {
      continue;
    }

    mInstances[Installed].Protocol.Revision    = EFI_SPI_CR_PROTOCOL_REVISION;
    mInstances[Installed].Protocol.HostHandle  = Handles[Index];
    mInstances[Installed].Protocol.Host        = Host;
    mInstances[Installed].Protocol.Attributes            = Host->Attributes;
    mInstances[Installed].Protocol.FrameSizeSupportMask  =
        Host->FrameSizeSupportMask;
    mInstances[Installed].Protocol.MaximumTransferBytes  =
        Host->MaximumTransferBytes;
    mInstances[Installed].Protocol.ChipSelect  = SpiCrChipSelect;
    mInstances[Installed].Protocol.Clock       = SpiCrClock;
    mInstances[Installed].Protocol.Transaction = SpiCrTransaction;

    AliasHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &AliasHandle, &gEfiSpiCrProtocolGuid,
        &mInstances[Installed].Protocol, NULL);
    if (EFI_ERROR (Status)) {
      continue;
    }
    mInstances[Installed].AliasHandle = AliasHandle;
    ++Installed;
  }

  FreePool (Handles);
  if (Installed == 0) {
    FreePool (mInstances);
    mInstances = NULL;
    return EFI_NOT_FOUND;
  }
  mInstanceCount = Installed;
  return EFI_SUCCESS;
}
