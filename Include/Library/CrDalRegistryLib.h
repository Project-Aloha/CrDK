/** @file
  Validation and lookup helpers for a Crane device manifest.

  SPDX-License-Identifier: MIT
**/
#ifndef CR_DAL_REGISTRY_LIB_H_
#define CR_DAL_REGISTRY_LIB_H_

#include <Library/CrDalProviderLib.h>

EFI_STATUS
CrDalRegistryValidate (
  IN CONST CR_DAL_DEVICE_MANIFEST *Manifest
  );

EFI_STATUS
CrDalRegistryGetDeviceInfo (
  IN  CONST CR_DAL_DEVICE_MANIFEST *Manifest,
  IN  CR_DAL_DEVICE_TYPE            Type,
  IN  UINT32                        Instance,
  IN  UINT32                        MinimumDataRevision,
  IN  UINTN                         MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO    **DeviceInfo
  );

EFI_STATUS
CrDalRegistryGetDeviceCount (
  IN  CONST CR_DAL_DEVICE_MANIFEST *Manifest,
  OUT UINTN                        *DeviceCount
  );

EFI_STATUS
CrDalRegistryGetDeviceByIndex (
  IN  CONST CR_DAL_DEVICE_MANIFEST *Manifest,
  IN  UINTN                         Index,
  OUT CONST CR_DAL_DEVICE_INFO    **DeviceInfo
  );

#endif
