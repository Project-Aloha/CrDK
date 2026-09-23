/** @file
  SM8150 target-data manifest consumed by CrDALDxe.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetLib.h>

STATIC CR_DAL_DEVICE_INFO mSm8150Devices[] = {
  {
    .Revision     = CR_DAL_DEVICE_INFO_REVISION,
    .Size         = sizeof (CR_DAL_DEVICE_INFO),
    .Type         = CrDalDeviceDebugUart,
    .Instance     = 0,
    .DataRevision = CR_DAL_DATA_REVISION_1,
    .Attributes   = CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE |
                    CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE |
                    CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES,
    .DataSize     = sizeof (CrDebugUartContext),
    .Data         = NULL,
  },
};

STATIC CONST CR_DAL_DEVICE_MANIFEST mSm8150Manifest = {
  .Revision    = CR_DAL_DEVICE_MANIFEST_REVISION,
  .Size        = sizeof (CR_DAL_DEVICE_MANIFEST),
  .DeviceCount = sizeof (mSm8150Devices) / sizeof (mSm8150Devices[0]),
  .Devices     = mSm8150Devices,
};

EFI_STATUS
CrTargetGetDeviceManifest (
  OUT CONST CR_DAL_DEVICE_MANIFEST **Manifest
  )
{
  if (Manifest == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  mSm8150Devices[0].Data = CrTargetGetDebugUartContext ();
  if (mSm8150Devices[0].Data == NULL) {
    *Manifest = NULL;
    return EFI_NOT_FOUND;
  }

  *Manifest = &mSm8150Manifest;
  return EFI_SUCCESS;
}
