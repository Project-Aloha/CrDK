/** @file
  Board-independent SM8550 resource provider for the shared Crane drivers.

  SPDX-License-Identifier: MIT
**/
#include <Library/CrTargetLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/PcdLib.h>

STATIC CONST CR_DAL_SOC_INFO mSocInfo = {
  .Revision = CR_DAL_SOC_INFO_REVISION,
  .Size = sizeof (CR_DAL_SOC_INFO),
  .QualcommSocId = 519,
  .Compatible = "qcom,sm8550",
};

#define STORAGE (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define MUTABLE (STORAGE | CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE)
#define IMMUTABLE (STORAGE | CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE)
#define DEVICE(Type, Payload, Attributes) \
  {CR_DAL_DEVICE_INFO_REVISION, sizeof (CR_DAL_DEVICE_INFO), (Type), 0, \
   CR_DAL_DATA_REVISION_1, (Attributes), sizeof (Payload), NULL}

STATIC CR_DAL_DEVICE_INFO mDevices[] = {
  {CR_DAL_DEVICE_INFO_REVISION, sizeof (CR_DAL_DEVICE_INFO), CrDalDeviceSocInfo,
   0, CR_DAL_SOC_INFO_REVISION, IMMUTABLE, sizeof (mSocInfo), &mSocInfo},
  DEVICE (CrDalDeviceGpio, GpioDeviceContext, MUTABLE),
  DEVICE (CrDalDevicePdc, PdcDeviceContext, MUTABLE),
  DEVICE (CrDalDeviceRpmh, RpmhDeviceContext, MUTABLE),
  // Keep optional UART last; default zero omits this device from the manifest.
  DEVICE (CrDalDeviceDebugUart, CrDebugUartContext, MUTABLE),
};
STATIC CR_DAL_DEVICE_MANIFEST mManifest = {
  CR_DAL_DEVICE_MANIFEST_REVISION, sizeof (CR_DAL_DEVICE_MANIFEST), 0, mDevices
};

EFI_STATUS
CrTargetGetDeviceManifest (OUT CONST CR_DAL_DEVICE_MANIFEST **Manifest)
{
  if (Manifest == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Manifest = NULL;
  if (mManifest.DeviceCount == 0) {
    CrDebugUartContext *Uart = CrTargetGetDebugUartContext ();
    // Reject an inherited base from a different SoC before publication.
    if ((FixedPcdGet32 (PcdUartSerialBase) != 0) && (Uart == NULL)) {
      return EFI_UNSUPPORTED;
    }
    mDevices[1].Data = CrTargetGetGpioContext ();
    mDevices[2].Data = GetPdcDevContext ();
    mDevices[3].Data = CrTargetGetRpmhContext ();
    mDevices[4].Data = Uart;
    mManifest.DeviceCount = ARRAY_SIZE (mDevices) - (Uart == NULL ? 1 : 0);
  }
  *Manifest = &mManifest;
  return EFI_SUCCESS;
}
