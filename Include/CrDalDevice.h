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
  CrDalDeviceRpmhRails         = 19,
  CrDalDevicePil               = 20,
  CrDalDeviceRpmhTopology      = 21,
  CrDalDeviceSd                = 22,
  CrDalDeviceUfs               = 23,
  CrDalDeviceRpmhArcInventory  = 24,
  CrDalDeviceMdss              = 25,
  CrDalDeviceSocInfo           = 26,
  CrDalDeviceMax               = 27
};

/** Declared SoC target identity, independent of the selected board.
    QualcommSocId follows public dt-bindings/arm/qcom,ids.h. This identifies
    the resource provider; it does not attest the running hardware. The
    compatible is a terminated public SoC DT compatible, never a board name.
    This optional immutable payload does not change the v1 descriptor stride.
 */
#define CR_DAL_SOC_INFO_REVISION  CR_DAL_REVISION (1, 0)
#define CR_DAL_SOC_COMPATIBLE_MAX 32U
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  UINT32 QualcommSocId;
  CHAR8  Compatible[CR_DAL_SOC_COMPATIBLE_MAX];
} CR_DAL_SOC_INFO;

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

/** Additional shared RPMh supplies beyond the legacy PCIe description.
    CmdDB provides addresses; consumers supply voltage/mode and own votes. */
typedef struct {
  UINTN Count;
  CONST CHAR8 *CONST *Names;
} CR_DAL_RPMH_RAILS;

/** Board input-supply dependencies. One parent lease per active child rail;
    the parent voltage/mode must agree for all children sharing that input. */
typedef struct {
  CONST CHAR8 *Child;
  CONST CHAR8 *Parent;
  UINT32       VoltageMv;
  UINT8        Mode;
} CR_DAL_RPMH_PARENT;

typedef struct {
  UINTN Count;
  CONST CR_DAL_RPMH_PARENT *Parents;
} CR_DAL_RPMH_TOPOLOGY;

/** Optional ARC boot inventory. Entries are ordered parent before child;
    ParentIndex is an earlier index or CR_DAL_RPMH_ARC_NO_PARENT. MinLevel
    qualifies the highest CmdDB corner; it is a virtual performance level,
    never a voltage or a hardware corner index. Acknowledged maximum floors
    remain permanent until reboot while native consumers are unsynchronized.
    Omission preserves the historical six-resource provider/PHOF contract. */
#define CR_DAL_RPMH_ARC_REVISION 1U
#define CR_DAL_RPMH_ARC_POWER 1U
#define CR_DAL_RPMH_ARC_XO 2U
#define CR_DAL_RPMH_ARC_NO_PARENT MAX_UINT32
typedef struct {
  CHAR8 Name[9];
  UINT32 Kind;
  UINT32 MinLevel;
  UINT32 ParentIndex;
} CR_DAL_RPMH_ARC_ENTRY;
typedef struct {
  UINT32 Revision;
  UINT32 Size;
  UINT32 Count;
  CONST CR_DAL_RPMH_ARC_ENTRY *Entries;
} CR_DAL_RPMH_ARC_INVENTORY;

typedef struct {
  UINT16        InterruptPin;
  UINTN         PowerPinCount;
  CONST UINT16 *PowerPins;
  UINT32        I2cInstance;
  UINT8         I2cAddress;
} CR_DAL_LT9611_CONFIG;

#endif
