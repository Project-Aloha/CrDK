/** @file
  Validation and lookup helpers for a Crane device manifest.

  SPDX-License-Identifier: MIT
**/

#include <Library/CrDalRegistryLib.h>

STATIC
BOOLEAN
CrDalRevisionCompatible (
  IN UINT32 Available,
  IN UINT32 Required
  )
{
  if (Required == 0) {
    return TRUE;
  }

  return (BOOLEAN)(
    (CR_DAL_REVISION_MAJOR (Available) == CR_DAL_REVISION_MAJOR (Required)) &&
    (CR_DAL_REVISION_MINOR (Available) >= CR_DAL_REVISION_MINOR (Required))
    );
}

EFI_STATUS
CrDalRegistryValidate (
  IN CONST CR_DAL_DEVICE_MANIFEST *Manifest
  )
{
  CONST CR_DAL_DEVICE_INFO *Device;
  UINTN                     Index;
  UINTN                     Other;
  UINT32                    Mutability;

  if ((Manifest == NULL) ||
      (Manifest->Size < sizeof (*Manifest)) ||
      !CrDalRevisionCompatible (
         Manifest->Revision,
         CR_DAL_DEVICE_MANIFEST_REVISION
         ) ||
      (Manifest->DeviceCount > MAX_UINTN / sizeof (CR_DAL_DEVICE_INFO)) ||
      ((Manifest->DeviceCount != 0) && (Manifest->Devices == NULL))) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Manifest->DeviceCount; Index++) {
    Device = &Manifest->Devices[Index];
    Mutability = Device->Attributes &
                 (CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE |
                  CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE);
    if ((Device->Size != sizeof (*Device)) ||
        !CrDalRevisionCompatible (
           Device->Revision,
           CR_DAL_DEVICE_INFO_REVISION
           ) ||
        (Device->Type <= CrDalDeviceInvalid) ||
        (Device->Type >= CrDalDeviceMax) ||
        (Device->DataRevision == 0) ||
        (Device->DataSize == 0) ||
        (Device->Data == NULL) ||
        ((Device->Attributes & ~CR_DAL_DEVICE_ATTRIBUTE_VALID_MASK) != 0) ||
        ((Device->Attributes &
          (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE |
           CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)) !=
         (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE |
          CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)) ||
        ((Mutability != CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE) &&
         (Mutability != CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE))) {
      return EFI_INVALID_PARAMETER;
    }

    for (Other = 0; Other < Index; Other++) {
      if ((Manifest->Devices[Other].Type == Device->Type) &&
          (Manifest->Devices[Other].Instance == Device->Instance)) {
        return EFI_ALREADY_STARTED;
      }
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
CrDalRegistryGetDeviceInfo (
  IN  CONST CR_DAL_DEVICE_MANIFEST *Manifest,
  IN  CR_DAL_DEVICE_TYPE            Type,
  IN  UINT32                        Instance,
  IN  UINT32                        MinimumDataRevision,
  IN  UINTN                         MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO    **DeviceInfo
  )
{
  CONST CR_DAL_DEVICE_INFO *Candidate;
  UINTN                     Index;

  if (DeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceInfo = NULL;
  if ((Type <= CrDalDeviceInvalid) || (Type >= CrDalDeviceMax)) {
    return EFI_INVALID_PARAMETER;
  }
  if (EFI_ERROR (CrDalRegistryValidate (Manifest))) {
    return EFI_INVALID_PARAMETER;
  }

  for (Index = 0; Index < Manifest->DeviceCount; Index++) {
    Candidate = &Manifest->Devices[Index];
    if ((Candidate->Type != Type) || (Candidate->Instance != Instance)) {
      continue;
    }

    if (!CrDalRevisionCompatible (
           Candidate->DataRevision,
           MinimumDataRevision
           )) {
      return EFI_INCOMPATIBLE_VERSION;
    }
    if (Candidate->DataSize < MinimumDataSize) {
      return EFI_BAD_BUFFER_SIZE;
    }

    *DeviceInfo = Candidate;
    return EFI_SUCCESS;
  }

  return EFI_NOT_FOUND;
}

EFI_STATUS
CrDalRegistryGetDeviceCount (
  IN  CONST CR_DAL_DEVICE_MANIFEST *Manifest,
  OUT UINTN                        *DeviceCount
  )
{
  if (DeviceCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceCount = 0;
  if (EFI_ERROR (CrDalRegistryValidate (Manifest))) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceCount = Manifest->DeviceCount;
  return EFI_SUCCESS;
}

EFI_STATUS
CrDalRegistryGetDeviceByIndex (
  IN  CONST CR_DAL_DEVICE_MANIFEST *Manifest,
  IN  UINTN                         Index,
  OUT CONST CR_DAL_DEVICE_INFO    **DeviceInfo
  )
{
  if (DeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  *DeviceInfo = NULL;
  if (EFI_ERROR (CrDalRegistryValidate (Manifest))) {
    return EFI_INVALID_PARAMETER;
  }
  if (Index >= Manifest->DeviceCount) {
    return EFI_NOT_FOUND;
  }

  *DeviceInfo = &Manifest->Devices[Index];
  return EFI_SUCCESS;
}
