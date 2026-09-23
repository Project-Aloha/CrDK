/** @file
  Crane target-data publication driver.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/CrDalRegistryLib.h>
#include <Library/CrTargetLib.h>
#include <Library/DebugLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Protocol/EFICrDalProtocol.h>

STATIC CONST CR_DAL_DEVICE_MANIFEST *mManifest;
STATIC EFI_HANDLE                    mCrDalHandle;

STATIC
EFI_STATUS
EFIAPI
CrDalGetDeviceInfo (
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  CR_DAL_DEVICE_TYPE        Type,
  IN  UINT32                    Instance,
  IN  UINT32                    MinimumDataRevision,
  IN  UINTN                     MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return CrDalRegistryGetDeviceInfo (
           mManifest,
           Type,
           Instance,
           MinimumDataRevision,
           MinimumDataSize,
           DeviceInfo
           );
}

STATIC
EFI_STATUS
EFIAPI
CrDalGetDeviceCount (
  IN  EFI_CR_DAL_PROTOCOL *This,
  OUT UINTN               *DeviceCount
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return CrDalRegistryGetDeviceCount (mManifest, DeviceCount);
}

STATIC
EFI_STATUS
EFIAPI
CrDalGetDeviceByIndex (
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  UINTN                     Index,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  if (This == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  return CrDalRegistryGetDeviceByIndex (mManifest, Index, DeviceInfo);
}

STATIC EFI_CR_DAL_PROTOCOL mCrDalProtocol = {
  .Revision         = EFI_CR_DAL_PROTOCOL_REVISION,
  .Size             = sizeof (EFI_CR_DAL_PROTOCOL),
  .Reserved         = 0,
  .GetDeviceInfo    = CrDalGetDeviceInfo,
  .GetDeviceCount   = CrDalGetDeviceCount,
  .GetDeviceByIndex = CrDalGetDeviceByIndex,
};

EFI_STATUS
EFIAPI
CrDalDxeEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE *SystemTable
  )
{
  EFI_STATUS Status;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  Status = CrTargetGetDeviceManifest (&mManifest);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CrDAL: target manifest unavailable: %r\n", Status));
    return Status;
  }

  Status = CrDalRegistryValidate (mManifest);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CrDAL: invalid target manifest: %r\n", Status));
    mManifest = NULL;
    return Status;
  }

  Status = gBS->InstallMultipleProtocolInterfaces (
                  &mCrDalHandle,
                  &gEfiCrDalProtocolGuid,
                  &mCrDalProtocol,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "CrDAL: protocol installation failed: %r\n", Status));
    mManifest = NULL;
  }

  return Status;
}
