/** @file
  Platform provider interface consumed only by CrDALDxe.

  SPDX-License-Identifier: MIT
**/
#ifndef CR_DAL_PROVIDER_LIB_H_
#define CR_DAL_PROVIDER_LIB_H_

#include <CrDalDevice.h>

#define CR_DAL_DEVICE_MANIFEST_REVISION  CR_DAL_REVISION (1, 0)

typedef struct {
  UINT32                    Revision;
  UINT32                    Size;
  UINTN                     DeviceCount;
  CONST CR_DAL_DEVICE_INFO *Devices;
} CR_DAL_DEVICE_MANIFEST;

/**
  Return the platform's resident device manifest.

  The provider owns the manifest, descriptors, payloads, child tables, and any
  callback code until ExitBootServices. It must complete descriptor setup
  before returning success and must not subsequently alter the manifest or
  descriptor metadata. Only payloads marked SHARED_MUTABLE may change, under
  the resource driver's synchronization rules. Missing devices are omitted.

  Version 1 descriptors have a fixed sizeof (CR_DAL_DEVICE_INFO) array stride.
  CrDALDxe is the only DXE image that links this provider, so clients never
  create independent copies of writable clock/GPIO/RPMh state.
**/
EFI_STATUS
CrTargetGetDeviceManifest (
  OUT CONST CR_DAL_DEVICE_MANIFEST **Manifest
  );

#endif
