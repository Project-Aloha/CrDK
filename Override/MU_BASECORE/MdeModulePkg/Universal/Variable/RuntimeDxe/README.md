# Crane-owned variable persistence override

This directory stages the selected `VariableRuntimeDxe.inf` source closure from
MU_BASECORE revision `562c0927ef7bfc65d26d9caf322e569ccb23b6a9`. The 23 selected
source, header, INF and UNI files preserve the module name, file GUID
`CBD2E4D5-7068-4FF5-B462-9822B4AD8D60`, entry point and existing source behavior.
The INF compiles each C source directly. Its quoted headers are all local to this
closure; package headers and libraries continue to come from MdePkg,
MdeModulePkg and CranePkg. SMM and StandaloneMm modules are outside this closure.

`SOURCE_INVENTORY.json` records upstream, pre-relocation local and staged SHA256
values for every copied file. Original copyright and BSD-2-Clause-Patent notices
remain intact; `LICENSE.txt` contains the upstream license. The local persistence
adapter and interface carry the same SPDX license. The existing local patch and
both exact upstream/local snapshots are retained under
`.tmp/waipio-continuation/nv-override71/`.

The local patch loads an emulated NV store before the existing variable parser
and authentication initialization, wraps the completed standard SetVariable
transaction, and saves authentication defaults after initialization. The existing
variable engine still performs authentication, policy, append/delete and
attribute checks. Persistence IO runs after its variable lock is released.
`CranePkg/Include/Library/VariablePersistenceLib.h` defines the optional backend;
`CranePkg/Library/EspVariablePersistenceLib` is the existing EFI ESP implementation.
`CranePkg/Library/VariablePersistenceLibNull` preserves the original no-op option.

The only relocation changes in copied production files are CranePkg package
declarations in the two INFs. This staging step leaves the official MU sources,
active DSC/FDF selections and firmware images unchanged. Root integration must
register the library class in CranePkg.dec, select this override and the moved
Null path where needed, then remove the archived local NV additions from the
official package. This directory is not an independent firmware activation.

Focused validation:

```
python3 Platforms/CranePkg/Tests/check_variable_override.py
python3 Platforms/CranePkg/Tests/run_variable_persistence_integration_tests.py --output .tmp/waipio-continuation/nv-override71/integration
python3 Platforms/CranePkg/Tests/run_esp_variable_persistence_tests.py --output .tmp/waipio-continuation/nv-override71/esp
```

These tests exercise the actual staged bootstrap/parser and transaction adapter,
the actual ESP backend against modeled firmware/filesystem services, and its
ARM64 object. They validate the moved header through compiler dependencies.
Physical reboot persistence and full override firmware integration require their
respective platform checks.
