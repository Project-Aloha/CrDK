# Crane device data service

`CrDALDxe` publishes the silicon data consumed by Crane DXE drivers through
`EFI_CR_DAL_PROTOCOL`. `CrDalLib` provides typed accessors for those descriptors.
Portable hardware libraries receive their context from the DXE resource owner;
they do not locate a UEFI protocol or link their own target implementation.

Only `CrDALDxe` links `CrTargetLib`. The selected provider implements
`CrTargetGetDeviceManifest()` and returns its complete set of available devices.
The provider initializes this metadata before publication. It does not cold
initialize PCIe or acquire its LDO, ICB, SMMU, clock, reset, or PHY resources.
The resource drivers and `PciHostBridgeLib` continue to own those operations.

The providers live in `CranePkg/Target/Sm8450` and `CranePkg/Target/Sm8150`.
Each platform selects its provider with `CR_TARGET_LIB_PATH`; the former
silicon-package target copies and headers have been removed. Platform memory
map and board PCD inputs are resolved by the provider before publication.

## Discovery contract

Each descriptor identifies a `(Type, Instance)` pair and carries a descriptor
revision, payload revision, payload size, and explicit storage attributes. The
current silicon descriptions use instance zero; more instances can be added
without changing a consumer which asks for a particular instance.

`GetDeviceInfo()` requires the requested payload's major revision to match and
its minor revision to meet the requested minimum. A zero requested revision
disables that check. A payload smaller than the requested size is rejected.
`GetDeviceCount()` and `GetDeviceByIndex()` enumerate the complete manifest,
including device types not used by a particular driver. Device type IDs must
be supported by the registry/client ABI version. A missing type,
instance, or out-of-range index returns `EFI_NOT_FOUND`.

Manifest ABI 1 uses an array stride of `sizeof(CR_DAL_DEVICE_INFO)`.
`CR_DAL_DEVICE_INFO.Size` must equal that value. Extending this descriptor
requires a new manifest major revision. Payload types can evolve independently
through their `DataRevision` and `DataSize` fields. The registry rejects duplicate
keys, invalid sizes, missing payloads, and inconsistent storage attributes
before the DXE protocol is installed. The client validates returned descriptors
again before giving a typed pointer to a driver.

## Storage and ownership

The provider image remains resident. The manifest, descriptors, payloads,
referenced child tables, and callback code remain valid until
`ExitBootServices()`. Consumers borrow those pointers and must not free them.
This service has no runtime API.

Immutable entries describe PCIe, SMMU, ICB, QUP/GPI, TRNG, and optional
PMIC GPIO/button data, the CMD DB memory region, and LT9611 board GPIOs.
The LT9611 descriptor is omitted on boards without a UINT16 power-pin array.
The existing clock, debug clock, debug UART, GPIO,
PDC, RPMh, and SPMI context ABIs can contain mutable initialization state. Such
entries carry `SHARED_MUTABLE`, and the resource owner must serialize changes.
They are published once so each DXE does not receive a separate copy of the
same resource state. A descriptor cannot be both immutable and shared mutable.

Clock platform callbacks are a separate, versioned `CR_DAL_CLOCK_SERVICES`
payload. Revision 1.1 adds `ResolveResource()`, which maps a Crane
`(Controller, Id, Kind)` tuple to the packaged driver's native ID and returns
resource flags. `NO_RATE` suppresses an unsupported exact-frequency request;
`EXTERNAL_SOURCE` selects native source zero before the first enable vote. This
also makes source/leaf aliases share one native reference. Waipio's
`ClockCrDxe` uses this resolver with the packaged Clock protocol, but does not
invoke the raw GCC initialization and reset callbacks intended for an
environment where Crane owns GCC. The PCIe I/O payload is immutable and
`CrDalGetPcieIo()` copies it to the host's local callback table before the host
installs resource-owner callbacks.

## Adding a consumer or provider

A DXE consumer links `CrDalLib`, includes `Library/CrDalLib.h`, and calls its
typed accessor. `CrDalLib` contributes `gEfiCrDalProtocolGuid` to the DXE
dependency expression. Resource dependencies such as clock or interconnect
readiness remain the consumer's own dependency expression. Portable library
entry points accept the returned context explicitly.

A provider lists only resources available on that silicon. An absent optional
SPMI, PMIC GPIO, or button entry does not require a stub getter or weak symbol.
The SM8450 manifest contains its clock/GPIO/RPMh and cold PCIe prerequisites;
the SM8150 manifest supplies the debug UART description used by that target.

Run the manifest and client ABI tests with:

```sh
TMPDIR=/var/tmp python3 Platforms/CranePkg/Tests/run_crdal_tests.py
```

These tests use the real registry/client code with EDK2 headers and address/
undefined-behavior sanitizers. They validate discovery and data contracts;
hardware bring-up remains a firmware/runtime validation task.
