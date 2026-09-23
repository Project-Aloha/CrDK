# Waipio PMIC resource evidence

The packaged PMIC resource definitions live in `DALSYSDxe.efi`; searching
only `PmicDxeLa.efi` or `NpaDxe.efi` does not find their names. Both Waipio
boards define scalar NPA resources `/pm/<rail>/mode`, `/pm/<rail>/mV`, and
`/pm/<rail>/en` for the PCIe rails `ldob5`, `ldob6`, and `ldoh2`.

The inspected files are under each board's
`Binaries/QcomPkg/Drivers/DALSYSDxe/DALSYSDxe.efi`:

| Board | SHA-256 |
| --- | --- |
| qcom-hdk8450 | `c970ad1138a7402dee7ca76d611c5f288458ee5b6fb375be8c2658dc26007f34` |
| qcom-mtp8450 | `d090b4189b6b51bd42aebcc44973dbc054c69a4020d0fae5da6bf0576908027a` |

These PE images have preferred image base zero, with matching section file
offsets and RVAs. The target records use the 96-byte AArch64 layout of
Qualcomm's `pm_prm_rsrc_data_type`. The resource-name pointer is at offset 0,
node-name pointer at 8, CMD DB name at 24, units at 32, PMIC chip at 40,
resource key at 56, and RPMh register offset at 68.

| Rail | HDK mode / voltage / enable record RVAs | MTP record RVAs |
| --- | --- | --- |
| ldob5 | `0x3af58` / `0x3afb8` / `0x3b018` | `0x3af18` / `0x3af78` / `0x3afd8` |
| ldob6 | `0x3b078` / `0x3b0d8` / `0x3b138` | `0x3b038` / `0x3b098` / `0x3b0f8` |
| ldoh2 | `0x3ba98` / `0x3baf8` / `0x3bb58` | `0x3ba58` / `0x3bab8` / `0x3bb18` |

For every rail, the three records point to the same CMD DB identity and
`/node/<rail>` node. Their units are `mode`, `mV`, and `on_off`; keys are
0, 1, and 2; register offsets are 8, 0, and 4. The peripheral indices are
4, 5, and 1 for `ldob5`, `ldob6`, and `ldoh2`, respectively.

The packaged `/pmic/client/pcie0` node depends on the `ldob5` and `ldob6`
resources. `/pmic/client/pcie1` depends on all three rails. Each resource
has required (`0x40`) and suppressible (`0x800`) dependency entries. This
confirms registration and shared use; Crane requests the individual rails
described by its Linux-derived target instead of assuming the aggregate
PCIe node's mode table matches that target.

The corresponding source contract is in Qualcomm's SM8150 boot source,
`QcomPkg/Library/PmicLib/prm/`:

- `inc/pm_prm_device_defs.h`, `PM_PRM_DEV_RSRC_VREG`, defines the names,
  units, keys and RPMh offsets above.
- `src/pm_prm_init.c`, `pm_prm_device_init`, registers the scalar resources
  with `pm_prm_scalar_driver_fcn` and maximum state `0xffff`.
- `src/pm_prm_os.c`, `pm_prm_os_set_device_plugin`, selects `npa_max_plugin`
  for RPMh-controlled resources, so distinct clients' requests aggregate.
- `inc/pm_prm_device.h` defines PMIC5 normal/high-power mode as 7.

Direct scalar requests have `pam_req == FALSE`; `pm_prm_scalar_driver_fcn`
posts RPMh commands and calls `pm_prm_barrier_request`. The active commands
request completion. Operational release therefore issues synchronous scalar
zero votes, starting with enable, and retains the clients for reuse.
`CompleteRequest` cannot substitute for this sequence: NPA's
`npa_terminate_request` sets `NPA_REQUEST_FIRE_AND_FORGET`, which causes the
PMIC barrier path to skip waiting. A zero vote removes only Crane's demand;
the max plugin retains other clients' requests.

## Dispatch readiness

`gQcomPmicVregProtocolGuid`
(`22d38d3d-e8b6-4f8f-9c26-bceb07d6cb68`) marks native PMIC initialization
for the adapter's dependency expression. Merely finding NPA is insufficient:
the PMIC resource nodes are registered later by the PMIC driver.

In both packaged `PmicDxeLa.efi` images, the entry implementation at RVA
`0x15d0` calls `pm_init` at `0x131cc` before the common protocol installer
at `0x16d4`. That installer passes the Vreg GUID to
`InstallMultipleProtocolInterfaces` at `0x187c`. The GUID is at `0x2a0b8`
in HDK and `0x2a0a8` in MTP. On the normal initialization path, HDK's
`pm_init` calls PRM initialization at `0x1320c` (target `0x14f2c`);
MTP calls it at `0x13300` (target `0x15098`). This matches
`Drivers/PmicDxe/Pmic.c` and `Library/PmicLib/framework/src/pm_core_init.c`.
Rail client creation still checks for missing resources, including a native
stub/standalone initialization which does not create the physical resources.

| PMIC binary | SHA-256 |
| --- | --- |
| qcom-hdk8450 | `fd0b224f78ffeb4bd6a169cb6309a05b6a2634ada87377523444b0faccfa0f1f` |
| qcom-mtp8450 | `5db3f57f120e706ff663fe6839ae27c4be614e5d7d34c36e887c1d8551f95546` |

Resource discovery and request completion still need validation on hardware.
Binary/source agreement does not establish rail voltage or settling time at
the board. A missing NPA resource must fail initialization rather than fall
back to uncoordinated RPMh writes.
