/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>

#undef NULL
#include <Library/CrDalLib.h>

#define IMMUTABLE_ATTRIBUTES \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | \
   CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE | \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)

EFI_GUID gEfiCrDalProtocolGuid = EFI_CR_DAL_PROTOCOL_GUID;
EFI_BOOT_SERVICES *gBS;

STATIC EFI_STATUS mLocateStatus = EFI_NOT_FOUND;
STATIC EFI_STATUS mProviderStatus = EFI_SUCCESS;
STATIC EFI_STATUS mCountStatus = EFI_SUCCESS;
STATIC CR_DAL_DEVICE_INFO mResponse;
STATIC BOOLEAN mReturnNull;
STATIC UINTN mLocateCount;

STATIC EFI_STATUS EFIAPI
MockGetDeviceInfo (
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  CR_DAL_DEVICE_TYPE        Type,
  IN  UINT32                    Instance,
  IN  UINT32                    MinimumDataRevision,
  IN  UINTN                     MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  (VOID)This;
  (VOID)Type;
  (VOID)Instance;
  (VOID)MinimumDataRevision;
  (VOID)MinimumDataSize;
  *DeviceInfo = mReturnNull ? NULL : &mResponse;
  return mProviderStatus;
}

STATIC EFI_STATUS EFIAPI
MockGetDeviceCount (
  IN  EFI_CR_DAL_PROTOCOL *This,
  OUT UINTN               *DeviceCount
  )
{
  (VOID)This;
  *DeviceCount = 1;
  return mCountStatus;
}

STATIC EFI_STATUS EFIAPI
MockGetDeviceByIndex (
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  UINTN                     Index,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  )
{
  (VOID)This;
  *DeviceInfo = &mResponse;
  return (Index == 0) ? EFI_SUCCESS : EFI_NOT_FOUND;
}

STATIC EFI_CR_DAL_PROTOCOL mProtocol = {
  .Revision = EFI_CR_DAL_PROTOCOL_REVISION,
  .Size = sizeof (EFI_CR_DAL_PROTOCOL),
  .GetDeviceInfo = MockGetDeviceInfo,
  .GetDeviceCount = MockGetDeviceCount,
  .GetDeviceByIndex = MockGetDeviceByIndex,
};

STATIC EFI_STATUS EFIAPI
MockLocateProtocol (
  IN  EFI_GUID *Protocol,
  IN  VOID     *Registration,
  OUT VOID    **Interface
  )
{
  (VOID)Registration;
  assert (Protocol == &gEfiCrDalProtocolGuid);
  mLocateCount++;
  *Interface = &mProtocol;
  return mLocateStatus;
}

STATIC EFI_BOOT_SERVICES mBootServices;

STATIC VOID
SetResponse (
  IN CR_DAL_DEVICE_TYPE Type,
  IN UINTN              Size,
  IN CONST VOID        *Data
  )
{
  mResponse.Revision = CR_DAL_DEVICE_INFO_REVISION;
  mResponse.Size = sizeof (mResponse);
  mResponse.Type = Type;
  mResponse.Instance = 0;
  mResponse.DataRevision = CR_DAL_DATA_REVISION_1;
  mResponse.Attributes = IMMUTABLE_ATTRIBUTES;
  mResponse.DataSize = Size;
  mResponse.Data = Data;
}

STATIC VOID
TestDiscovery (
  VOID
  )
{
  CONST CR_DAL_DEVICE_INFO *Info;
  UINTN Count = 99;

  gBS = NULL;
  assert (CrDalGetDeviceCount (&Count) == EFI_NOT_READY);
  assert (Count == 0);
  gBS = &mBootServices;
  assert (CrDalGetDeviceCount (&Count) == EFI_NOT_READY);
  mBootServices.LocateProtocol = MockLocateProtocol;
  assert (CrDalGetDeviceCount (&Count) == EFI_NOT_FOUND);
  assert (Count == 0);
  mLocateStatus = EFI_SUCCESS;
  mProtocol.Revision = CR_DAL_REVISION (2, 0);
  assert (CrDalGetDeviceCount (&Count) == EFI_INCOMPATIBLE_VERSION);
  mProtocol.Revision = EFI_CR_DAL_PROTOCOL_REVISION;
  mProtocol.Size--;
  assert (CrDalGetDeviceCount (&Count) == EFI_INCOMPATIBLE_VERSION);
  mProtocol.Size = sizeof (mProtocol);
  mProtocol.GetDeviceInfo = NULL;
  assert (CrDalGetDeviceCount (&Count) == EFI_INCOMPATIBLE_VERSION);
  mProtocol.GetDeviceInfo = MockGetDeviceInfo;
  assert (CrDalGetDeviceCount (&Count) == EFI_SUCCESS);
  assert (Count == 1);
  Count = mLocateCount;
  assert (CrDalGetDeviceInfo (
            CrDalDeviceTrng, 0, 0, 0, &Info) == EFI_COMPROMISED_DATA);
  assert (Info == NULL);
  assert (mLocateCount == Count);
}

STATIC VOID
TestTypedPayloadValidation (
  VOID
  )
{
  CR_TRNG_CONFIG Config = { .BaseAddress = 0x10c3000, .MmioSize = 0x1000 };
  CR_DAL_DEVICE_INFO Original;
  CONST CR_DAL_DEVICE_INFO *Info;
  ClockDriverContext Clock = { 0 };

  SetResponse (CrDalDeviceTrng, sizeof (Config), &Config);
  assert (CrDalGetTrngConfig () == &Config);
  Original = mResponse;

  mReturnNull = TRUE;
  assert (CrDalGetTrngConfig () == NULL);
  mReturnNull = FALSE;
  mResponse.Type = CrDalDeviceSmmu;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.Instance = 1;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.Revision = CR_DAL_REVISION (2, 0);
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.Size++;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.DataRevision = CR_DAL_REVISION (2, 0);
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.DataSize--;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.Data = NULL;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.Attributes &= ~CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;
  mResponse.Attributes |= CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE;
  assert (CrDalGetTrngConfig () == NULL);
  mResponse = Original;

  mProviderStatus = EFI_NOT_FOUND;
  assert (CrDalGetDeviceInfo (
            CrDalDeviceTrng, 0, 0, 0, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
  mProviderStatus = EFI_SUCCESS;

  SetResponse (CrDalDeviceClock, sizeof (Clock), &Clock);
  assert (CrDalGetClockContext () == NULL);
  mResponse.Attributes &= ~CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE;
  mResponse.Attributes |= CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE;
  assert (CrDalGetClockContext () == &Clock);
}

STATIC UINTN mClockCalls;
STATIC ClockDriverContext *mClockExpected;

STATIC EFI_STATUS EFIAPI
MockClockInitialize (
  IN ClockDriverContext *ClockContext
  )
{
  assert (ClockContext == mClockExpected);
  mClockCalls++;
  return EFI_DEVICE_ERROR;
}

STATIC EFI_STATUS EFIAPI
MockClockReset (
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  )
{
  assert (Controller != NULL && Id != NULL && Assert);
  mClockCalls++;
  return EFI_SUCCESS;
}

STATIC EFI_STATUS EFIAPI
MockClockResolve (
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  )
{
  assert (Controller != NULL && Id != NULL);
  assert (Kind == CrDalClockResourceClock);
  *NativeId = "native_clock";
  *Flags = CR_DAL_CLOCK_RESOURCE_NO_RATE;
  mClockCalls++;
  return EFI_SUCCESS;
}

STATIC VOID
TestServiceAndCopyContracts (
  VOID
  )
{
  ClockDriverContext Clock = { 0 };
  CR_DAL_CLOCK_SERVICES Services = {
    .Revision = CR_DAL_CLOCK_SERVICES_REVISION,
    .Size = sizeof (CR_DAL_CLOCK_SERVICES),
    .Initialize = MockClockInitialize,
    .SetReset = MockClockReset,
    .ResolveResource = MockClockResolve,
  };
  PcieIoOps Template = { .Context = &Clock };
  PcieIoOps Copy = { 0 };
  CONST CHAR8 *NativeId = NULL;
  UINT32 Flags = 0;

  mClockExpected = &Clock;
  SetResponse (CrDalDeviceClockServices, sizeof (Services), &Services);
  assert (CrDalClockInit (NULL) == EFI_INVALID_PARAMETER);
  assert (CrDalClockReset (NULL, "reset", TRUE) == EFI_INVALID_PARAMETER);
  assert (CrDalClockResolveResource (
            "gcc", "clock", CrDalClockResourceMax, &NativeId, &Flags
            ) == EFI_INVALID_PARAMETER);
  assert (mClockCalls == 0);
  assert (CrDalClockInit (&Clock) == EFI_DEVICE_ERROR);
  assert (CrDalClockReset ("gcc", "reset", TRUE) == EFI_SUCCESS);
  assert (CrDalClockResolveResource (
            "gcc", "clock", CrDalClockResourceClock, &NativeId, &Flags
            ) == EFI_SUCCESS);
  assert (NativeId != NULL && Flags == CR_DAL_CLOCK_RESOURCE_NO_RATE);
  assert (mClockCalls == 3);
  Services.Size--;
  assert (CrDalClockInit (&Clock) == EFI_NOT_FOUND);
  assert (mClockCalls == 3);
  Services.Size = sizeof (Services);
  Services.SetReset = NULL;
  assert (CrDalClockInit (&Clock) == EFI_NOT_FOUND);
  assert (mClockCalls == 3);
  Services.SetReset = MockClockReset;
  Services.ResolveResource = NULL;
  assert (CrDalClockInit (&Clock) == EFI_NOT_FOUND);
  assert (mClockCalls == 3);

  SetResponse (CrDalDevicePcieIo, sizeof (Template), &Template);
  assert (CrDalGetPcieIo (NULL) == EFI_INVALID_PARAMETER);
  assert (CrDalGetPcieIo (&Copy) == EFI_SUCCESS);
  assert (Copy.Context == Template.Context);
  Copy.Context = NULL;
  assert (Template.Context == &Clock);
}

STATIC VOID
TestEnumeration (
  VOID
  )
{
  UINT64 Payload = 1;
  CONST CR_DAL_DEVICE_INFO *Info;
  UINTN Count = 99;

  SetResponse (CrDalDeviceTrng, sizeof (Payload), &Payload);
  assert (CrDalGetDeviceCount (&Count) == EFI_SUCCESS && Count == 1);
  assert (CrDalGetDeviceByIndex (0, &Info) == EFI_SUCCESS);
  assert (Info == &mResponse);
  assert (CrDalGetDeviceByIndex (1, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
  mResponse.Data = NULL;
  assert (CrDalGetDeviceByIndex (0, &Info) == EFI_COMPROMISED_DATA);
  assert (Info == NULL);
  mCountStatus = EFI_DEVICE_ERROR;
  assert (CrDalGetDeviceCount (&Count) == EFI_DEVICE_ERROR && Count == 0);
  assert (CrDalGetDeviceCount (NULL) == EFI_INVALID_PARAMETER);
  assert (CrDalGetDeviceByIndex (0, NULL) == EFI_INVALID_PARAMETER);
}

int
main (void)
{
  TestDiscovery ();
  TestTypedPayloadValidation ();
  TestServiceAndCopyContracts ();
  TestEnumeration ();
  puts ("CrDAL client: discovery retry, typed payload validation, service and copy contracts passed");
  return 0;
}
