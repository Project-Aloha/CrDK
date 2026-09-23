/** @file
  Typed accessors for data published by CrDALDxe.

  SPDX-License-Identifier: MIT
**/

#include <Library/CrDalLib.h>
#include <Library/UefiBootServicesTableLib.h>

STATIC EFI_CR_DAL_PROTOCOL *mCrDal;

STATIC
EFI_STATUS
CrDalLocateProtocol (
  VOID
  )
{
  EFI_STATUS Status;

  if (mCrDal != NULL) {
    return EFI_SUCCESS;
  }
  if ((gBS == NULL) || (gBS->LocateProtocol == NULL)) {
    return EFI_NOT_READY;
  }

  Status = gBS->LocateProtocol (
                  &gEfiCrDalProtocolGuid,
                  NULL,
                  (VOID **)&mCrDal
                  );
  if (EFI_ERROR (Status)) {
    mCrDal = NULL;
    return Status;
  }
  if ((mCrDal == NULL) ||
      ((mCrDal->Revision >> 16) != (EFI_CR_DAL_PROTOCOL_REVISION >> 16)) ||
      (mCrDal->Revision < EFI_CR_DAL_PROTOCOL_REVISION) ||
      (mCrDal->Size < sizeof (*mCrDal)) ||
      (mCrDal->GetDeviceInfo == NULL) ||
      (mCrDal->GetDeviceCount == NULL) ||
      (mCrDal->GetDeviceByIndex == NULL)) {
    mCrDal = NULL;
    return EFI_INCOMPATIBLE_VERSION;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
CrDalValidateDeviceInfo (
  IN CONST CR_DAL_DEVICE_INFO *DeviceInfo
  )
{
  UINT32 Mutability;

  if ((DeviceInfo == NULL) ||
      (DeviceInfo->Size != sizeof (*DeviceInfo)) ||
      (CR_DAL_REVISION_MAJOR (DeviceInfo->Revision) !=
       CR_DAL_REVISION_MAJOR (CR_DAL_DEVICE_INFO_REVISION)) ||
      (DeviceInfo->Revision < CR_DAL_DEVICE_INFO_REVISION) ||
      (DeviceInfo->Type <= CrDalDeviceInvalid) ||
      (DeviceInfo->Type >= CrDalDeviceMax) ||
      (DeviceInfo->Data == NULL) ||
      (DeviceInfo->DataSize == 0) ||
      (DeviceInfo->DataRevision == 0) ||
      ((DeviceInfo->Attributes & ~CR_DAL_DEVICE_ATTRIBUTE_VALID_MASK) != 0) ||
      ((DeviceInfo->Attributes &
        (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE |
         CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)) !=
       (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE |
        CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES))) {
    return EFI_COMPROMISED_DATA;
  }

  Mutability = DeviceInfo->Attributes &
               (CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE |
                CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE);
  if ((Mutability != CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE) &&
      (Mutability != CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE)) {
    return EFI_COMPROMISED_DATA;
  }
  return EFI_SUCCESS;
}

EFI_STATUS
CrDalGetDeviceInfo (
  IN  CR_DAL_DEVICE_TYPE        Type,
  IN  UINT32                    Instance,
  IN  UINT32                    MinimumDataRevision,
  IN  UINTN                     MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  EFI_STATUS Status;

  if (DeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *DeviceInfo = NULL;
  if ((Type <= CrDalDeviceInvalid) || (Type >= CrDalDeviceMax)) {
    return EFI_INVALID_PARAMETER;
  }

  Status = CrDalLocateProtocol ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = mCrDal->GetDeviceInfo (
                    mCrDal,
                    Type,
                    Instance,
                    MinimumDataRevision,
                    MinimumDataSize,
                    DeviceInfo
                    );
  if (!EFI_ERROR (Status)) {
    Status = CrDalValidateDeviceInfo (*DeviceInfo);
  }
  if (!EFI_ERROR (Status) &&
      (((*DeviceInfo)->Type != Type) || ((*DeviceInfo)->Instance != Instance))) {
    Status = EFI_COMPROMISED_DATA;
  }
  if (!EFI_ERROR (Status) && (MinimumDataRevision != 0) &&
      ((CR_DAL_REVISION_MAJOR ((*DeviceInfo)->DataRevision) !=
        CR_DAL_REVISION_MAJOR (MinimumDataRevision)) ||
       ((*DeviceInfo)->DataRevision < MinimumDataRevision))) {
    Status = EFI_INCOMPATIBLE_VERSION;
  }
  if (!EFI_ERROR (Status) && ((*DeviceInfo)->DataSize < MinimumDataSize)) {
    Status = EFI_BAD_BUFFER_SIZE;
  }
  if (EFI_ERROR (Status)) {
    *DeviceInfo = NULL;
  }
  return Status;
}

EFI_STATUS
CrDalGetDeviceCount (
  OUT UINTN *DeviceCount
  )
{
  EFI_STATUS Status;

  if (DeviceCount == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *DeviceCount = 0;

  Status = CrDalLocateProtocol ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mCrDal->GetDeviceCount (mCrDal, DeviceCount);
  if (EFI_ERROR (Status)) {
    *DeviceCount = 0;
  }
  return Status;
}

EFI_STATUS
CrDalGetDeviceByIndex (
  IN  UINTN                     Index,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  EFI_STATUS Status;

  if (DeviceInfo == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  *DeviceInfo = NULL;

  Status = CrDalLocateProtocol ();
  if (EFI_ERROR (Status)) {
    return Status;
  }
  Status = mCrDal->GetDeviceByIndex (mCrDal, Index, DeviceInfo);
  if (!EFI_ERROR (Status)) {
    Status = CrDalValidateDeviceInfo (*DeviceInfo);
  }
  if (EFI_ERROR (Status)) {
    *DeviceInfo = NULL;
  }
  return Status;
}

STATIC
CONST VOID *
CrDalGetSingleton (
  IN CR_DAL_DEVICE_TYPE Type,
  IN UINTN              DataSize,
  IN UINT32             Mutability
  )
{
  CONST CR_DAL_DEVICE_INFO *DeviceInfo;

  if (EFI_ERROR (CrDalGetDeviceInfo (
                   Type,
                   0,
                   CR_DAL_DATA_REVISION_1,
                   DataSize,
                   &DeviceInfo
                   ))) {
    return NULL;
  }
  if ((DeviceInfo == NULL) || (DeviceInfo->Data == NULL) ||
      ((DeviceInfo->Attributes & Mutability) != Mutability)) {
    return NULL;
  }
  return DeviceInfo->Data;
}

RpmhDeviceContext *
CrDalGetRpmhContext (
  VOID
  )
{
  return (RpmhDeviceContext *)(UINTN)CrDalGetSingleton (
                                      CrDalDeviceRpmh,
                                      sizeof (RpmhDeviceContext),
                                      CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                      );
}

ClockDriverContext *
CrDalGetClockContext (
  VOID
  )
{
  return (ClockDriverContext *)(UINTN)CrDalGetSingleton (
                                       CrDalDeviceClock,
                                       sizeof (ClockDriverContext),
                                       CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                       );
}

DebugccDriverContext *
CrDalGetDebugClockContext (
  VOID
  )
{
  return (DebugccDriverContext *)(UINTN)CrDalGetSingleton (
                                         CrDalDeviceDebugClock,
                                         sizeof (DebugccDriverContext),
                                         CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                         );
}

CrDebugUartContext *
CrDalGetDebugUartContext (
  VOID
  )
{
  return (CrDebugUartContext *)(UINTN)CrDalGetSingleton (
                                       CrDalDeviceDebugUart,
                                       sizeof (CrDebugUartContext),
                                       CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                       );
}

GpioDeviceContext *
CrDalGetGpioContext (
  VOID
  )
{
  return (GpioDeviceContext *)(UINTN)CrDalGetSingleton (
                                      CrDalDeviceGpio,
                                      sizeof (GpioDeviceContext),
                                      CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                      );
}

PdcDeviceContext *
CrDalGetPdcContext (
  VOID
  )
{
  return (PdcDeviceContext *)(UINTN)CrDalGetSingleton (
                                     CrDalDevicePdc,
                                     sizeof (PdcDeviceContext),
                                     CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                     );
}

CONST CR_TRNG_CONFIG *
CrDalGetTrngConfig (
  VOID
  )
{
  return (CONST CR_TRNG_CONFIG *)CrDalGetSingleton (
                                  CrDalDeviceTrng,
                                  sizeof (CR_TRNG_CONFIG),
                                  CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                  );
}

CONST PcieTargetContext *
CrDalGetPcieContext (
  VOID
  )
{
  return (CONST PcieTargetContext *)CrDalGetSingleton (
                                      CrDalDevicePcie,
                                      sizeof (PcieTargetContext),
                                      CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                      );
}

CONST InterconnectTargetContext *
CrDalGetInterconnectContext (
  VOID
  )
{
  return (CONST InterconnectTargetContext *)CrDalGetSingleton (
                                              CrDalDeviceInterconnect,
                                              sizeof (InterconnectTargetContext),
                                              CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                              );
}

CONST CrTargetSmmuContext *
CrDalGetSmmuContext (
  VOID
  )
{
  return (CONST CrTargetSmmuContext *)CrDalGetSingleton (
                                          CrDalDeviceSmmu,
                                          sizeof (CrTargetSmmuContext),
                                          CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                          );
}

CONST CR_TARGET_QUP_CONTEXT *
CrDalGetQupContext (
  VOID
  )
{
  return (CONST CR_TARGET_QUP_CONTEXT *)CrDalGetSingleton (
                                           CrDalDeviceQup,
                                           sizeof (CR_TARGET_QUP_CONTEXT),
                                           CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                           );
}

SpmiDeviceContext *
CrDalGetSpmiContext (
  VOID
  )
{
  return (SpmiDeviceContext *)(UINTN)CrDalGetSingleton (
                                      CrDalDeviceSpmi,
                                      sizeof (SpmiDeviceContext),
                                      CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE
                                      );
}

CONST PmicGpioTargetContext *
CrDalGetPmicGpioContext (
  VOID
  )
{
  return (CONST PmicGpioTargetContext *)CrDalGetSingleton (
                                          CrDalDevicePmicGpio,
                                          sizeof (PmicGpioTargetContext),
                                          CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                          );
}

CONST ButtonsTargetContext *
CrDalGetButtonsContext (
  VOID
  )
{
  return (CONST ButtonsTargetContext *)CrDalGetSingleton (
                                       CrDalDeviceButtons,
                                       sizeof (ButtonsTargetContext),
                                       CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                       );
}

STATIC
CONST CR_DAL_CLOCK_SERVICES *
CrDalGetClockServices (
  VOID
  )
{
  CONST CR_DAL_CLOCK_SERVICES *Services;

  Services = (CONST CR_DAL_CLOCK_SERVICES *)CrDalGetSingleton (
                                                  CrDalDeviceClockServices,
                                                  sizeof (*Services),
                                                  CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                                  );
  if ((Services == NULL) ||
      (Services->Size < sizeof (*Services)) ||
      (CR_DAL_REVISION_MAJOR (Services->Revision) !=
       CR_DAL_REVISION_MAJOR (CR_DAL_CLOCK_SERVICES_REVISION)) ||
      (CR_DAL_REVISION_MINOR (Services->Revision) <
       CR_DAL_REVISION_MINOR (CR_DAL_CLOCK_SERVICES_REVISION)) ||
      (Services->Initialize == NULL) ||
      (Services->SetReset == NULL) ||
      (Services->ResolveResource == NULL)) {
    return NULL;
  }
  return Services;
}

CONST CR_DAL_CMD_DB_CONFIG *
CrDalGetCmdDbConfig (
  VOID
  )
{
  return (CONST CR_DAL_CMD_DB_CONFIG *)CrDalGetSingleton (
                                       CrDalDeviceCmdDb,
                                       sizeof (CR_DAL_CMD_DB_CONFIG),
                                       CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                       );
}

CONST CR_DAL_LT9611_CONFIG *
CrDalGetLt9611Config (
  VOID
  )
{
  return (CONST CR_DAL_LT9611_CONFIG *)CrDalGetSingleton (
                                       CrDalDeviceLt9611,
                                       sizeof (CR_DAL_LT9611_CONFIG),
                                       CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                       );
}

CR_STATUS
CrDalClockInit (
  IN ClockDriverContext *ClockContext
  )
{
  CONST CR_DAL_CLOCK_SERVICES *Services;

  if (ClockContext == NULL) {
    return CR_INVALID_PARAMETER;
  }
  Services = CrDalGetClockServices ();
  return (Services == NULL) ? CR_NOT_FOUND : Services->Initialize (ClockContext);
}

CR_STATUS
CrDalClockReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  CONST CR_DAL_CLOCK_SERVICES *Services;

  if ((Controller == NULL) || (Id == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  Services = CrDalGetClockServices ();
  return (Services == NULL) ? CR_NOT_FOUND :
                              Services->SetReset (Controller, Id, Assert);
}

CR_STATUS
CrDalClockResolveResource (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  CONST CR_DAL_CLOCK_SERVICES *Services;

  if ((Controller == NULL) || (Id == NULL) ||
      (Kind >= CrDalClockResourceMax) || (NativeId == NULL) ||
      (Flags == NULL)) {
    return CR_INVALID_PARAMETER;
  }
  *NativeId = NULL;
  *Flags    = 0;
  Services = CrDalGetClockServices ();
  return (Services == NULL) ? CR_NOT_FOUND :
                              Services->ResolveResource (
                                          Controller,
                                          Id,
                                          Kind,
                                          NativeId,
                                          Flags
                                          );
}

EFI_STATUS
CrDalGetPcieIo (
  OUT PcieIoOps *Io
  )
{
  CONST PcieIoOps *PublishedIo;

  if (Io == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  PublishedIo = (CONST PcieIoOps *)CrDalGetSingleton (
                                      CrDalDevicePcieIo,
                                      sizeof (PcieIoOps),
                                      CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE
                                      );
  if (PublishedIo == NULL) {
    return EFI_NOT_FOUND;
  }

  *Io = *PublishedIo;
  return EFI_SUCCESS;
}
