/** @file
 *  Alias the standard MU I2C master protocol for Crane consumers.
 *
 *  The MU I2CDxe binary remains responsible for GENI programming.  This
 *  adapter discovers every standard EFI_I2C_MASTER_PROTOCOL handle and
 *  publishes a corresponding Crane protocol on a separate handle.  It does
 *  not invent or bind to a private Qualcomm ABI.
 *
 *  SPDX-License-Identifier: MIT
 */

#include <Uefi.h>

#include <Library/MemoryAllocationLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiDriverEntryPoint.h>

#include <Protocol/EFIGpiCrProtocol.h>
#include <Protocol/EFII2cCrProtocol.h>
#include <Protocol/EFIQupCrProtocol.h>

typedef struct {
  EFI_HANDLE       AliasHandle;
  EFI_I2C_CR_PROTOCOL Protocol;
} I2C_CR_INSTANCE;

STATIC I2C_CR_INSTANCE *mInstances;
STATIC UINTN            mInstanceCount;

STATIC I2C_CR_INSTANCE *
I2cCrInstanceFromProtocol (
  IN EFI_I2C_CR_PROTOCOL *This
  )
{
  if (This == NULL) {
    return NULL;
  }
  return BASE_CR (This, I2C_CR_INSTANCE, Protocol);
}

STATIC EFI_STATUS
EFIAPI
I2cCrSetBusFrequency (
  IN EFI_I2C_CR_PROTOCOL *This,
  IN OUT UINTN           *BusClockHertz
  )
{
  I2C_CR_INSTANCE *Instance;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Master == NULL) ||
      (Instance->Protocol.Master->SetBusFrequency == NULL)) {
    return EFI_NOT_READY;
  }
  return Instance->Protocol.Master->SetBusFrequency (
      Instance->Protocol.Master, BusClockHertz);
}

STATIC EFI_STATUS
EFIAPI
I2cCrReset (
  IN EFI_I2C_CR_PROTOCOL *This
  )
{
  I2C_CR_INSTANCE *Instance;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Master == NULL) ||
      (Instance->Protocol.Master->Reset == NULL)) {
    return EFI_NOT_READY;
  }
  return Instance->Protocol.Master->Reset (Instance->Protocol.Master);
}

STATIC EFI_STATUS
EFIAPI
I2cCrStartRequest (
  IN EFI_I2C_CR_PROTOCOL   *This,
  IN UINTN                  SlaveAddress,
  IN EFI_I2C_REQUEST_PACKET *RequestPacket,
  IN EFI_EVENT              Event OPTIONAL,
  OUT EFI_STATUS           *I2cStatus OPTIONAL
  )
{
  I2C_CR_INSTANCE *Instance;

  Instance = I2cCrInstanceFromProtocol (This);
  if ((Instance == NULL) || (Instance->Protocol.Master == NULL) ||
      (Instance->Protocol.Master->StartRequest == NULL)) {
    return EFI_NOT_READY;
  }
  return Instance->Protocol.Master->StartRequest (
      Instance->Protocol.Master, SlaveAddress, RequestPacket, Event,
      I2cStatus);
}

EFI_STATUS
EFIAPI
I2CCrEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS                    Status;
  EFI_HANDLE                   *Handles;
  UINTN                         HandleCount;
  UINTN                         Index;
  UINTN                         Installed;
  EFI_I2C_MASTER_PROTOCOL      *Master;

  (VOID)SystemTable;
  Handles = NULL;
  HandleCount = 0;
  Installed = 0;

  Status = gBS->LocateHandleBuffer (
      ByProtocol, &gEfiI2cMasterProtocolGuid, NULL,
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

    Master = NULL;
    Status = gBS->OpenProtocol (
        Handles[Index], &gEfiI2cMasterProtocolGuid, (VOID **)&Master,
        ImageHandle, NULL, EFI_OPEN_PROTOCOL_GET_PROTOCOL);
    if (EFI_ERROR (Status) || (Master == NULL) ||
        (Master->SetBusFrequency == NULL) || (Master->Reset == NULL) ||
        (Master->StartRequest == NULL)) {
      continue;
    }

    mInstances[Installed].Protocol.Revision        = EFI_I2C_CR_PROTOCOL_REVISION;
    mInstances[Installed].Protocol.MasterHandle    = Handles[Index];
    mInstances[Installed].Protocol.Master          = Master;
    mInstances[Installed].Protocol.I2cControllerCapabilities =
        Master->I2cControllerCapabilities;
    mInstances[Installed].Protocol.SetBusFrequency = I2cCrSetBusFrequency;
    mInstances[Installed].Protocol.Reset           = I2cCrReset;
    mInstances[Installed].Protocol.StartRequest    = I2cCrStartRequest;

    AliasHandle = NULL;
    Status = gBS->InstallMultipleProtocolInterfaces (
        &AliasHandle, &gEfiI2cCrProtocolGuid,
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
