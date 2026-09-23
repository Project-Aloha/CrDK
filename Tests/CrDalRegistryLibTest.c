/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>

#undef NULL
#include <Library/CrDalRegistryLib.h>

#define IMMUTABLE_ATTRIBUTES \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE | \
   CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE | \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)

STATIC CONST UINT64 mPayload[] = { 0x123456789abcdef0ULL, 0x8877665544332211ULL };

STATIC CR_DAL_DEVICE_INFO mDevices[] = {
  {
    .Revision = CR_DAL_DEVICE_INFO_REVISION,
    .Size = sizeof (CR_DAL_DEVICE_INFO),
    .Type = CrDalDeviceTrng,
    .Instance = 0,
    .DataRevision = CR_DAL_REVISION (1, 2),
    .Attributes = IMMUTABLE_ATTRIBUTES,
    .DataSize = sizeof (mPayload),
    .Data = mPayload,
  },
  {
    .Revision = CR_DAL_DEVICE_INFO_REVISION,
    .Size = sizeof (CR_DAL_DEVICE_INFO),
    .Type = CrDalDeviceTrng,
    .Instance = 1,
    .DataRevision = CR_DAL_REVISION (1, 0),
    .Attributes = IMMUTABLE_ATTRIBUTES,
    .DataSize = sizeof (mPayload[0]),
    .Data = &mPayload[1],
  },
};

STATIC CR_DAL_DEVICE_MANIFEST mManifest = {
  .Revision = CR_DAL_DEVICE_MANIFEST_REVISION,
  .Size = sizeof (CR_DAL_DEVICE_MANIFEST),
  .DeviceCount = sizeof (mDevices) / sizeof (mDevices[0]),
  .Devices = mDevices,
};

STATIC VOID
TestManifestValidation (
  VOID
  )
{
  CR_DAL_DEVICE_INFO     OriginalDevice;
  CR_DAL_DEVICE_MANIFEST OriginalManifest;

  assert (CrDalRegistryValidate (NULL) == EFI_INVALID_PARAMETER);
  assert (CrDalRegistryValidate (&mManifest) == EFI_SUCCESS);
  OriginalManifest = mManifest;

  mManifest.Devices = NULL;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mManifest.DeviceCount = 0;
  assert (CrDalRegistryValidate (&mManifest) == EFI_SUCCESS);
  mManifest = OriginalManifest;

  mManifest.Size--;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mManifest = OriginalManifest;
  mManifest.Revision = CR_DAL_REVISION (2, 0);
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mManifest = OriginalManifest;
  mManifest.DeviceCount = MAX_UINTN;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mManifest = OriginalManifest;

  OriginalDevice = mDevices[1];
  mDevices[1].Instance = 0;
  assert (CrDalRegistryValidate (&mManifest) == EFI_ALREADY_STARTED);
  mDevices[1] = OriginalDevice;

  mDevices[1].Data = NULL;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].DataSize = 0;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].DataRevision = 0;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Size = 0;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Size++;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Revision = CR_DAL_REVISION (2, 0);
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Type = CrDalDeviceInvalid;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Type = CrDalDeviceMax;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;

  mDevices[1].Attributes &= ~CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Attributes &= ~CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Attributes |= CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
  mDevices[1].Attributes |= 0x80000000U;
  assert (CrDalRegistryValidate (&mManifest) == EFI_INVALID_PARAMETER);
  mDevices[1] = OriginalDevice;
}

STATIC VOID
TestLookupContract (
  VOID
  )
{
  CONST CR_DAL_DEVICE_INFO *Info;

  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 0, CR_DAL_REVISION (1, 1),
            sizeof (mPayload), &Info) == EFI_SUCCESS);
  assert (Info == &mDevices[0]);
  assert (Info->Data == mPayload);
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 1, CR_DAL_DATA_REVISION_1,
            sizeof (mPayload[0]), &Info) == EFI_SUCCESS);
  assert (Info == &mDevices[1]);
  assert (Info->Data == &mPayload[1]);

  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceClock, 0, 0, 0, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 2, 0, 0, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 0, CR_DAL_REVISION (1, 3),
            0, &Info) == EFI_INCOMPATIBLE_VERSION);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 0, CR_DAL_REVISION (2, 0),
            0, &Info) == EFI_INCOMPATIBLE_VERSION);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 0, 0,
            sizeof (mPayload) + 1, &Info) == EFI_BAD_BUFFER_SIZE);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceTrng, 0, 0, 0, NULL) == EFI_INVALID_PARAMETER);
  Info = &mDevices[0];
  assert (CrDalRegistryGetDeviceInfo (
            &mManifest, CrDalDeviceInvalid, 0, 0, 0,
            &Info) == EFI_INVALID_PARAMETER);
  assert (Info == NULL);
}

STATIC VOID
TestEnumerationContract (
  VOID
  )
{
  CONST CR_DAL_DEVICE_INFO *Info;
  CR_DAL_DEVICE_MANIFEST    Empty;
  UINTN                     Count;
  UINTN                     Index;

  assert (CrDalRegistryGetDeviceCount (&mManifest, &Count) == EFI_SUCCESS);
  assert (Count == 2);
  for (Index = 0; Index < Count; Index++) {
    assert (CrDalRegistryGetDeviceByIndex (
              &mManifest, Index, &Info) == EFI_SUCCESS);
    assert (Info == &mDevices[Index]);
  }
  assert (CrDalRegistryGetDeviceByIndex (
            &mManifest, Count, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceByIndex (
            &mManifest, MAX_UINTN, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
  assert (CrDalRegistryGetDeviceCount (&mManifest, NULL) == EFI_INVALID_PARAMETER);
  assert (CrDalRegistryGetDeviceByIndex (
            &mManifest, 0, NULL) == EFI_INVALID_PARAMETER);

  Empty = mManifest;
  Empty.DeviceCount = 0;
  Empty.Devices = NULL;
  assert (CrDalRegistryGetDeviceCount (&Empty, &Count) == EFI_SUCCESS);
  assert (Count == 0);
  assert (CrDalRegistryGetDeviceByIndex (&Empty, 0, &Info) == EFI_NOT_FOUND);
  assert (Info == NULL);
}

int
main (void)
{
  TestManifestValidation ();
  TestLookupContract ();
  TestEnumerationContract ();
  puts ("CrDAL: manifest, duplicate, lifetime, version, size and enumeration checks passed");
  return 0;
}
