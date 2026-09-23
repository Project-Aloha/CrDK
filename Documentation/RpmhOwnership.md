# Waipio RPMh and PMIC rail ownership

`RpmhCrDxe` is an adapter to Qualcomm's packaged NPA/PMIC PRM stack. It is not
an RPMh hardware driver. The OEM `RpmhDxe` remains the sole HLOS DRV/TCS owner,
and Crane does not initialize the RPMh register block, register its IRQ, or
write raw TCS commands. `EFI_RPMH_CR_PROTOCOL.RpmhWrite` therefore returns
`EFI_UNSUPPORTED`.

## Dispatch readiness

The adapter depends on three protocols: CrDAL target data, OEM NPA, and the
native PMIC VREG protocol `22d38d3d-e8b6-4f8f-9c26-bceb07d6cb68`. Static
analysis of both packaged Waipio `PmicDxeLa.efi` images shows that this VREG
protocol is installed after `pm_init()` calls `pm_prm_init()` on the normal
hardware path. Rail client creation also rejects absent resources on a native
stub/standalone path. Requiring the protocol in DEPEX prevents a one-shot dispatch while NPA
exists but the PMIC scalar resources have not yet been registered.

The binary hashes, resource descriptors, and source cross-checks are recorded
in [WaipioPmicResources.md](WaipioPmicResources.md).

## NPA client model

The driver takes RPMh supply IDs from the immutable PCIe target published by
CrDAL. It deduplicates the shared `ldob6` entry and creates one persistent
`NPA_CLIENT_REQUIRED` client for each of these scalar resources per rail:

- `/pm/<rail>/mV`
- `/pm/<rail>/mode`
- `/pm/<rail>/en`

The PMIC PRM resources use `npa_max_plugin`. A Crane vote therefore aggregates
with native firmware clients instead of replacing another client's RPMh state.
The clients remain allocated for reuse, and a per-rail reference count keeps
the shared `ldob6` vote active until its final Crane user releases it.

PCIe cold initialization requests voltage, then PMIC5 mode, then enable. The
adapter accepts voltage states from 1 through `0xffff` and the PMIC5 modes
2 (bypass), 3 (retention), 4 (LPM), 6 (auto), and 7 (NPM). Mutable rail state
and NPA calls are serialized at `TPL_NOTIFY`.

For rollback, `RpmhEnableVreg(FALSE)` releases the entire Crane rail request in
reverse order: enable, mode, then voltage. It uses synchronous
`ScalarRequest(client, 0)` operations and stops at the first failure, retaining
that request and every prerequisite below it for a retry. This also releases a
voltage or mode vote when cold initialization fails before enable was issued.
A nonzero request is marked owned before calling the native wrapper because an
error return does not prove that no side effect occurred.

The operational release path deliberately does not call
`NpaCompleteRequest`. Qualcomm's NPA implementation routes that API through
`npa_terminate_request()`, which sets `NPA_REQUEST_FIRE_AND_FORGET`; PMIC PRM
then skips its RPMh barrier. A scalar zero request keeps aggregation semantics
while taking the direct PMIC PRM path that posts the active/sleep sets and
waits at `pm_prm_barrier_request()`.

## Error boundary

The native EFI NPA scalar wrapper calls the underlying void NPA API and then
returns `EFI_SUCCESS` unconditionally. Crane propagates EFI errors from the
protocol ABI and its retry state is tested with an injected failing wrapper,
but the packaged wrapper cannot report an internal RPMh hardware timeout or
PMIC PRM failure as an EFI status. Qualcomm code may instead assert or log a
fatal error. A successful request or firmware build is consequently not proof
that a rail reached its requested voltage or mode on either board.

Board validation still needs NPA/RPMh tracing and rail measurements across a
full PCIe cold start and every rollback stage. It must also confirm that both
PCIe ports can share and independently release `ldob6` without disturbing
native firmware clients.
