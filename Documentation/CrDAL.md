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
PMIC GPIO/button data, the CMD DB memory region, LT9611 board GPIOs and
the SM8450 ADSP PIL carveout, firmware path and interprocessor resource IDs.
The LT9611 descriptor is omitted on boards without a UINT16 power-pin array.
The existing clock, debug clock, debug UART, GPIO,
PDC, RPMh, and SPMI context ABIs can contain mutable initialization state. Such
entries carry `SHARED_MUTABLE`, and the resource owner must serialize changes.
They are published once so each DXE does not receive a separate copy of the
same resource state. A descriptor cannot be both immutable and shared mutable.

Clock platform callbacks are a separate, versioned `CR_DAL_CLOCK_SERVICES`
payload. Revision 1.1 adds `ResolveResource()`, which maps a Crane
`(Controller, Id, Kind)` tuple to the target's native node name and returns
resource flags. `NO_RATE` suppresses an unsupported exact-frequency request;
`EXTERNAL_SOURCE` selects source zero before the first enable vote for resources
that declare that behavior. The SM8450 PCIe PHY AUX aliases use `NO_RATE` only,
so ordinary enable preserves the bootloader-selected legal mux parent while an
explicit source-configuration call can change it while disabled. This also
makes source/leaf aliases share one native reference. On SM8450 `ClockCrDxe`
uses this resolver with Crane's own `ClockLib`; it does not locate or call the
Qualcomm `ClockDxe` protocol. The PCIe I/O payload is immutable and
`CrDalGetPcieIo()` copies it to the host's local callback table before the host
installs resource-owner callbacks.

## ABL device-tree handoff

`BootModeCaptureCrLib` preserves the raw ABL DTB and the bounded initrd tail
needed for Android bootconfig. SEC performs address/length checks and a minimal
`/chosen` initrd-bound lookup; UFS rail policy and Android mode parsing run in
DXE. No changes to MU's FDT library are required.

The copy arena is excluded from PHIT free memory before `HobConstructor` and
published as a named `EfiBootServicesData` allocation before the general heap
allocation HOB. `gCrBootDataGuid` describes the exact copied ranges and records
FDT and initrd-tail capture status separately. Original physical addresses are
source labels, not permission for a DXE consumer to reread ABL memory.

Crane PrePi starts the HOB list at `PHIT.EfiMemoryBottom`. MU's DXE Core
relocates the list into pool without changing its physical PHIT fields.
DAL validates the historical bounds and computes the copied list length from
them, then walks that length at the current `GetHobList()` address. It never
reads the reclaimed original HOB storage. The arena must be disjoint from both
list locations and the historical PEI free interval.

`CrDALDxe` checks the HOB record, allocation ownership and system-memory coverage,
then creates its own immutable snapshot. It uses MU `FdtLib`/libfdt for complete
validation and exposes `EFI_CR_FDT_PROTOCOL` alongside the device-data protocol.
Callers can obtain the blob, locate a path or compatible node, read a property,
or copy the parsed Android boot information. Returned blob/property pointers
belong to DAL and are valid until `ExitBootServices()`; there is no mutation or
runtime interface.

UFS policy is resolved from this snapshot before its target entry is published.
Rejected FDT data omits the dependent UFS entry while preserving unrelated DAL
entries. A bad initrd tail leaves a valid FDT available for UFS and reports an
error through `GetBootInfo`. `ChargerBootCrLib` obtains charging mode through
that method instead of consuming a SEC-parsed mode HOB.

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
