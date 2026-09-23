/** @file
  Common ABI for Crane device target data.

  SPDX-License-Identifier: MIT
**/
#ifndef CR_DAL_DEVICE_H_
#define CR_DAL_DEVICE_H_

#include <Uefi.h>

#define CR_DAL_REVISION(Major, Minor) \
  ((((UINT32)(Major)) << 16) | ((UINT32)(Minor) & 0xFFFFU))
#define CR_DAL_REVISION_MAJOR(Revision)  ((UINT16)((Revision) >> 16))
#define CR_DAL_REVISION_MINOR(Revision)  ((UINT16)((Revision) & 0xFFFFU))

#define CR_DAL_DEVICE_INFO_REVISION  CR_DAL_REVISION (1, 0)
#define CR_DAL_DATA_REVISION_1       CR_DAL_REVISION (1, 0)

/**
  Stable device identifiers.  Values are explicit because they are part of
  the protocol ABI.  Instance zero identifies each current singleton target.
**/
typedef UINT32 CR_DAL_DEVICE_TYPE;

enum {
  CrDalDeviceInvalid           = 0,
  CrDalDeviceRpmh              = 1,
  CrDalDeviceClock             = 2,
  CrDalDeviceClockServices     = 3,
  CrDalDeviceDebugClock        = 4,
  CrDalDeviceDebugUart         = 5,
  CrDalDeviceGpio              = 6,
  CrDalDevicePdc               = 7,
  CrDalDeviceTrng              = 8,
  CrDalDevicePcie              = 9,
  CrDalDevicePcieIo            = 10,
  CrDalDeviceInterconnect      = 11,
  CrDalDeviceSmmu              = 12,
  CrDalDeviceQup               = 13,
  CrDalDeviceSpmi              = 14,
  CrDalDevicePmicGpio          = 15,
  CrDalDeviceButtons           = 16,
  CrDalDeviceCmdDb             = 17,
  CrDalDeviceLt9611            = 18,
  CrDalDeviceMax               = 19
};

/** Data is backed by storage in the provider image. */
#define CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE       0x00000001U
/** Callers must not modify the returned object or any child tables. */
#define CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE             0x00000002U
/** Legacy context contains state intentionally shared by its DXE consumers. */
#define CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE        0x00000004U
/** Data and callbacks are valid during UEFI boot services only. */
#define CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES         0x00000008U
#define CR_DAL_DEVICE_ATTRIBUTE_VALID_MASK             \
  (CR_DAL_DEVICE_ATTRIBUTE_STATIC_STORAGE |            \
   CR_DAL_DEVICE_ATTRIBUTE_IMMUTABLE |                 \
   CR_DAL_DEVICE_ATTRIBUTE_SHARED_MUTABLE |            \
   CR_DAL_DEVICE_ATTRIBUTE_BOOT_SERVICES)

/**
  Version 1 manifests store a contiguous C array with a fixed stride of
  sizeof (CR_DAL_DEVICE_INFO). Size therefore must equal that value; an ABI
  which extends this descriptor must use a new manifest major revision.
**/
typedef struct {
  UINT32             Revision;
  UINT32             Size;
  CR_DAL_DEVICE_TYPE Type;
  UINT32             Instance;
  UINT32             DataRevision;
  UINT32             Attributes;
  UINTN              DataSize;
  CONST VOID        *Data;
} CR_DAL_DEVICE_INFO;

struct _ClockDriverContext;

typedef EFI_STATUS (EFIAPI *CR_DAL_CLOCK_INITIALIZE)(
  IN struct _ClockDriverContext *ClockContext
  );

typedef EFI_STATUS (EFIAPI *CR_DAL_CLOCK_SET_RESET)(
  IN CONST CHAR8 *Controller,
  IN CONST CHAR8 *Id,
  IN BOOLEAN      Assert
  );

typedef enum {
  CrDalClockResourceClock = 0,
  CrDalClockResourcePowerDomain,
  CrDalClockResourceReset,
  CrDalClockResourceMax
} CR_DAL_CLOCK_RESOURCE_KIND;

/** The native clock object is a gate or external source with no rate vote. */
#define CR_DAL_CLOCK_RESOURCE_NO_RATE  0x00000001U
/** Select native external source zero before taking the first enable vote. */
#define CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE  0x00000002U
#define CR_DAL_CLOCK_RESOURCE_VALID_FLAGS \
  (CR_DAL_CLOCK_RESOURCE_NO_RATE | CR_DAL_CLOCK_RESOURCE_EXTERNAL_SOURCE)

typedef EFI_STATUS (EFIAPI *CR_DAL_CLOCK_RESOLVE_RESOURCE)(
  IN  CONST CHAR8                 *Controller,
  IN  CONST CHAR8                 *Id,
  IN  CR_DAL_CLOCK_RESOURCE_KIND   Kind,
  OUT CONST CHAR8                **NativeId,
  OUT UINT32                      *Flags
  );

typedef struct {
  UINT32                         Revision;
  UINT32                         Size;
  CR_DAL_CLOCK_INITIALIZE        Initialize;
  CR_DAL_CLOCK_SET_RESET         SetReset;
  CR_DAL_CLOCK_RESOLVE_RESOURCE  ResolveResource;
} CR_DAL_CLOCK_SERVICES;

#define CR_DAL_CLOCK_SERVICES_REVISION  CR_DAL_REVISION (1, 1)

typedef struct {
  UINT64 BaseAddress;
  UINT64 Size;
} CR_DAL_CMD_DB_CONFIG;

typedef struct {
  UINT16        InterruptPin;
  UINTN         PowerPinCount;
  CONST UINT16 *PowerPins;
} CR_DAL_LT9611_CONFIG;

#endif
