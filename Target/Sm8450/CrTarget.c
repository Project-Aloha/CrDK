/** @file
  SM8450 target-data manifest consumed by CrDALDxe.

  SPDX-License-Identifier: MIT
**/

#include <Uefi.h>

#include <Library/CrTargetClockLib.h>
#include <Library/CrTargetDebugUartLib.h>
#include <Library/CrTargetGpioLib.h>
#include <Library/CrTargetInterconnectLib.h>
#include <Library/CrTargetLib.h>
#include <Library/CrTargetPcieLib.h>
#include <Library/CrTargetPdcLib.h>
#include <Library/CrTargetQupLib.h>
#include <Library/CrTargetRpmhLib.h>
#include <Library/CrTargetSmmuLib.h>
#include <Library/CrTargetTrngLib.h>
#include <Library/MemoryMapHelperLib.h>
#include <Library/PcdLib.h>
#include <Library/pcie.h>

#define CR_DAL_STATIC_IMMUTABLE \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | \
   CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE | \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)
#define CR_DAL_STATIC_MUTABLE \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | \
   CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE | \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)

enum {
  Sm8450DalRpmh,
  Sm8450DalClock,
  Sm8450DalClockServices,
  Sm8450DalDebugClock,
  Sm8450DalDebugUart,
  Sm8450DalGpio,
  Sm8450DalPdc,
  Sm8450DalTrng,
  Sm8450DalPcie,
  Sm8450DalPcieIo,
  Sm8450DalInterconnect,
  Sm8450DalSmmu,
  Sm8450DalQup,
  Sm8450DalCmdDb,
  // Optional board device must remain last in the manifest.
  Sm8450DalLt9611,
  Sm8450DalCount
};

STATIC
EFI_STATUS
EFIAPI
Sm8450ClockInitialize (
  IN ClockDriverContext *ClockContext
  )
{
  return CrTargetClockInit (ClockContext);
}

STATIC
EFI_STATUS
EFIAPI
Sm8450ClockSetReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  return CrTargetClockReset (Controller, Id, Assert);
}

STATIC
EFI_STATUS
EFIAPI
Sm8450ClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  return CrTargetClockResolveResource (
           Controller,
           Id,
           Kind,
           NativeId,
           Flags
           );
}

STATIC CONST CR_DAL_CLOCK_SERVICES mSm8450ClockServices = {
  .Revision        = CR_DAL_CLOCK_SERVICES_REVISION,
  .Size            = sizeof (CR_DAL_CLOCK_SERVICES),
  .Initialize      = Sm8450ClockInitialize,
  .SetReset        = Sm8450ClockSetReset,
  .ResolveResource = Sm8450ClockResolveResource,
};

STATIC PcieIoOps mSm8450PcieIo;
STATIC CR_DAL_CMD_DB_CONFIG mSm8450CmdDb;
STATIC CR_DAL_LT9611_CONFIG mSm8450Lt9611;

#define CR_DAL_ENTRY(DeviceType, PayloadType, DeviceAttributes) \
  { \
    .Revision     = CR_DAL_DEVICE_INFO_REVISION, \
    .Size         = sizeof (CR_DAL_DEVICE_INFO), \
    .Type         = (DeviceType), \
    .Instance     = 0, \
    .DataRevision = CR_DAL_DATA_REVISION_1, \
    .Attributes   = (DeviceAttributes), \
    .DataSize     = sizeof (PayloadType), \
    .Data         = NULL, \
  }

STATIC CR_DAL_DEVICE_INFO mSm8450Devices[Sm8450DalCount] = {
  [Sm8450DalRpmh] =
    CR_DAL_ENTRY (CrDalDeviceRpmh, RpmhDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalClock] =
    CR_DAL_ENTRY (CrDalDeviceClock, ClockDriverContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalClockServices] =
    CR_DAL_ENTRY (
      CrDalDeviceClockServices,
      CR_DAL_CLOCK_SERVICES,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalDebugClock] =
    CR_DAL_ENTRY (
      CrDalDeviceDebugClock,
      DebugccDriverContext,
      CR_DAL_STATIC_MUTABLE
      ),
  [Sm8450DalDebugUart] =
    CR_DAL_ENTRY (
      CrDalDeviceDebugUart,
      CrDebugUartContext,
      CR_DAL_STATIC_MUTABLE
      ),
  [Sm8450DalGpio] =
    CR_DAL_ENTRY (CrDalDeviceGpio, GpioDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalPdc] =
    CR_DAL_ENTRY (CrDalDevicePdc, PdcDeviceContext, CR_DAL_STATIC_MUTABLE),
  [Sm8450DalTrng] =
    CR_DAL_ENTRY (CrDalDeviceTrng, CR_TRNG_CONFIG, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalPcie] =
    CR_DAL_ENTRY (CrDalDevicePcie, PcieTargetContext, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalPcieIo] =
    CR_DAL_ENTRY (CrDalDevicePcieIo, PcieIoOps, CR_DAL_STATIC_IMMUTABLE),
  [Sm8450DalInterconnect] =
    CR_DAL_ENTRY (
      CrDalDeviceInterconnect,
      InterconnectTargetContext,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalSmmu] =
    CR_DAL_ENTRY (
      CrDalDeviceSmmu,
      CrTargetSmmuContext,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalQup] =
    CR_DAL_ENTRY (
      CrDalDeviceQup,
      CR_TARGET_QUP_CONTEXT,
      CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalCmdDb] =
    CR_DAL_ENTRY (
      CrDalDeviceCmdDb, CR_DAL_CMD_DB_CONFIG, CR_DAL_STATIC_IMMUTABLE
      ),
  [Sm8450DalLt9611] =
    CR_DAL_ENTRY (
      CrDalDeviceLt9611, CR_DAL_LT9611_CONFIG, CR_DAL_STATIC_IMMUTABLE
      ),
};

STATIC CR_DAL_DEVICE_MANIFEST mSm8450Manifest = {
  .Revision    = CR_DAL_DEVICE_MANIFEST_REVISION,
  .Size        = sizeof (CR_DAL_DEVICE_MANIFEST),
  .DeviceCount = Sm8450DalCount,
  .Devices     = mSm8450Devices,
};

EFI_STATUS
CrTargetGetDeviceManifest (
  OUT CONST CR_DAL_DEVICE_MANIFEST **Manifest
  )
{
  STATIC BOOLEAN Initialized;
  EFI_STATUS     Status;
  UINTN          Index;
  ARM_MEMORY_REGION_DESCRIPTOR_EX CmdDbRegion;
  UINTN          PowerPinsSize;

  if (Manifest == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *Manifest = NULL;

  if (!Initialized) {
    Status = LocateMemoryMapAreaByName ("AOP CMD DB", &CmdDbRegion);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mSm8450CmdDb.BaseAddress = CmdDbRegion.Address;
    mSm8450CmdDb.Size = CmdDbRegion.Length;
    mSm8450Devices[Sm8450DalCmdDb].Data = &mSm8450CmdDb;

    PowerPinsSize = FixedPcdGetSize (PcdLt9611GpioHigh);
    mSm8450Manifest.DeviceCount = Sm8450DalLt9611;
    if ((PowerPinsSize != 0) && ((PowerPinsSize % sizeof (UINT16)) == 0)) {
      mSm8450Lt9611.InterruptPin = FixedPcdGet16 (PcdLt9611GpioInt);
      mSm8450Lt9611.PowerPinCount = PowerPinsSize / sizeof (UINT16);
      mSm8450Lt9611.PowerPins = FixedPcdGetPtr (PcdLt9611GpioHigh);
      mSm8450Devices[Sm8450DalLt9611].Data = &mSm8450Lt9611;
      mSm8450Manifest.DeviceCount++;
    }
    mSm8450Devices[Sm8450DalRpmh].Data = CrTargetGetRpmhContext ();
    mSm8450Devices[Sm8450DalClock].Data = CrTargetGetClockContext ();
    mSm8450Devices[Sm8450DalClockServices].Data = &mSm8450ClockServices;
    mSm8450Devices[Sm8450DalDebugClock].Data = CrTargetGetDebugccContext ();
    mSm8450Devices[Sm8450DalDebugUart].Data = CrTargetGetDebugUartContext ();
    mSm8450Devices[Sm8450DalGpio].Data = CrTargetGetGpioContext ();
    mSm8450Devices[Sm8450DalPdc].Data = GetPdcDevContext ();
    mSm8450Devices[Sm8450DalTrng].Data = CrTargetGetTrngConfig ();
    mSm8450Devices[Sm8450DalPcie].Data = CrTargetGetPcieContext ();
    Status = CrTargetGetPcieIo (&mSm8450PcieIo);
    if (EFI_ERROR (Status)) {
      return Status;
    }
    mSm8450Devices[Sm8450DalPcieIo].Data = &mSm8450PcieIo;
    mSm8450Devices[Sm8450DalInterconnect].Data =
      CrTargetGetInterconnectContext ();
    mSm8450Devices[Sm8450DalSmmu].Data = CrTargetGetSmmuContext ();
    mSm8450Devices[Sm8450DalQup].Data = CrTargetGetQupContext ();

    for (Index = 0; Index < mSm8450Manifest.DeviceCount; Index++) {
      if (mSm8450Devices[Index].Data == NULL) {
        return EFI_NOT_FOUND;
      }
    }
    Initialized = TRUE;
  }

  *Manifest = &mSm8450Manifest;
  return EFI_SUCCESS;
}
