/** @file
  Read-only discovery protocol for Crane platform target data.

  SPDX-License-Identifier: MIT
**/
#ifndef EFI_CR_DAL_PROTOCOL_H_
#define EFI_CR_DAL_PROTOCOL_H_

#include <CrDalDevice.h>

#define EFI_CR_DAL_PROTOCOL_REVISION  0x0000000000010000ULL
#define EFI_CR_DAL_PROTOCOL_GUID \
  { 0x8bb34c93, 0x194d, 0x4f37, \
    { 0x87, 0x04, 0x25, 0x69, 0x42, 0x9b, 0xc2, 0x4d } }

extern EFI_GUID gEfiCrDalProtocolGuid;

typedef struct _EFI_CR_DAL_PROTOCOL EFI_CR_DAL_PROTOCOL;

/**
  Return one provider-owned device descriptor.

  The returned descriptor and Data pointer remain valid until ExitBootServices.
  MinimumDataRevision zero disables revision checking.  Compatible revisions
  must have the same major version and a minor version at least as new as the
  requested one.
**/
typedef EFI_STATUS (EFIAPI *EFI_CR_DAL_GET_DEVICE_INFO)(
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  CR_DAL_DEVICE_TYPE        Type,
  IN  UINT32                    Instance,
  IN  UINT32                    MinimumDataRevision,
  IN  UINTN                     MinimumDataSize,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  );

typedef EFI_STATUS (EFIAPI *EFI_CR_DAL_GET_DEVICE_COUNT)(
  IN  EFI_CR_DAL_PROTOCOL *This,
  OUT UINTN               *DeviceCount
  );

typedef EFI_STATUS (EFIAPI *EFI_CR_DAL_GET_DEVICE_BY_INDEX)(
  IN  EFI_CR_DAL_PROTOCOL      *This,
  IN  UINTN                     Index,
  OUT CONST CR_DAL_DEVICE_INFO **DeviceInfo
  );

struct _EFI_CR_DAL_PROTOCOL {
  UINT64                         Revision;
  UINT32                         Size;
  UINT32                         Reserved;
  EFI_CR_DAL_GET_DEVICE_INFO     GetDeviceInfo;
  EFI_CR_DAL_GET_DEVICE_COUNT    GetDeviceCount;
  EFI_CR_DAL_GET_DEVICE_BY_INDEX GetDeviceByIndex;
};

#endif
