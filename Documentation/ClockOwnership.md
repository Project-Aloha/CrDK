# Waipio clock ownership

`ClockCrDxe` is the sole GCC/dispcc owner on the independent SM8450 path. It
gets the shared context from `CrDAL`, initializes the Crane `ClockLib`, and
programs the Linux-derived register descriptions in `Library/Sm8450ClockLib`;
`Target/Sm8450` retains the native resource alias adapter. It does
not locate, link, or require Qualcomm `ClockDxe`; the active HDK8450 DSC/FDF
and APRIORI lists leave the OEM clock image out of the firmware.

The Crane clock protocol keeps the resource ABI used by the other Crane
drivers. Clock aliases are resolved to one target node and use one
reference-counted vote. GDSC, reset, source selection, divider programming,
QUP wrapper clocks, UFS clocks, and the SM8450 PCIe clocks all execute through
the same Crane-owned backend.

## Historical binary evidence

The following hashes document the old Qualcomm binaries that were previously
present in the platform tree. They are retained as provenance only and are not
part of the independent active path.

The active HDK and MTP `PatchedBinaries/ClockDxe.efi` files are identical:

```
4ac5cf05dec3f990f3afc71889ee9b134fd2502af30abadd55f280a68644c5a2
```

The HDK original binary has SHA256
`c394b76595273dd8ab0bce5f8e1b84ec973a87963f546cb2a5e900b9e95bc1d9`
and the same relevant layout. In both layouts, the PCIe PIPE records are:

| Resource | Name field RVA | Domain pointer field | Domain RVA | Mux register |
| --- | ---: | ---: | ---: | ---: |
| `gcc_pcie_0_pipe_clk` | `0x22fc0` | `0x22fc8` | `0x29938` | `0x17b060` |
| `gcc_pcie_1_pipe_clk` | `0x23160` | `0x23168` | `0x29ad8` | `0x19d064` |

Both domains have a null BSP pointer and zero BSP length. Their DomainMux
`ConfigMux` routine at RVA `0xf374` reads only byte zero of the supplied mux
configuration and writes that selector directly to the mux register. It does
not read the rate, divider, M, N, or twice-D fields. Thus the native call

```
SelectExternalSource (Clock, Id, 0, 0, 0, 0, 0, 0)
```

selects source zero; divider zero is ignored by this simple mux. Linux
`phy_mux_enable()` selects that external PHY input before enabling the PIPE
clock. A plain native `EnableClock` does not perform this mux write.

`gcc_pcie_1_phy_aux_clk` is also a no-BSP mux:

| Binary | Name string RVA | Name field RVA | Domain pointer field | Domain RVA | BSP pointer/length |
| --- | ---: | ---: | ---: | ---: | --- |
| HDK original | `0x15cc2` | `0x23090` | `0x23098` | `0x29a08` | `NULL / 0` at `0x29a08/0x29a10` |
| active patched HDK/MTP | `0x15cc2` | `0x23090` | `0x23098` | `0x29a08` | `NULL / 0` at `0x29a08/0x29a10` |
| MTP original (`e9c75b7c1b3ee97afcbfe5ecc6d251f7ec06a260509a32ec178ec75234558c0e`) | `0x154ea` | - | - | `0x289c8` | `NULL / 0` at `0x289c8/0x289d0` |

In the active/HDK layout, its HAL CGR address is `0x19d080` and the pmControl
table at `0x2f0e8` points to `ConfigMux` RVA `0xf374` and `DetectMux` RVA
`0xf390`. The original MTP table at `0x2e0a8` points to the equivalent routines
at `0xeea0` and `0xeebc`. Each `ConfigMux` is the same two-operation body:
`ldrb w9, [x1]` reads only HAL configuration byte zero (`nMuxSel`), then
`str w9, [x8]` writes it to the CGR. It never reads byte one (`nDiv2x`). The
corresponding detect routine reads the selector from the register and returns a
fixed `nDiv2x` of two. This also proves that divider zero in the external-source
request is ignored by this mux. Qualcomm's BOOT.MXF.2.5.1 HAL source at commit
`c60ac2f1da8da430f246da52f375e1d986e77349` has the same
`HAL_clk_GenericConfigDomainMux` implementation.

Qualcomm `Clock_SetClockFrequency` returns `DAL_ERROR_NOT_SUPPORTED` when the
BSP pointer is null. Linux describes its mux at `0x9d080`: selector zero is the
external QMP PHY AUX clock, selector two is XO, and QMP registers the external
clock at a fixed 20 MHz. Therefore both Crane IDs
`gcc_pcie_1_phy_aux_clk_src` and `gcc_pcie_1_phy_aux_clk` resolve to the native
leaf with `NO_RATE`. The adapter skips the unsupported exact 20 MHz request and
preserves the existing legal selector (PHY value zero or XO value two) before
the first native enable vote. The PCIe consumer now explicitly manages the
AUX parent lifecycle through the appended `SetParent` protocol operation
(revision `0x10002`). It selects XO before enabling AUX, switches to PHY only
after QMP readiness, and restores XO before teardown. The operation allows
ordinary mux changes with at most one active consumer, rejects shared or
uncertain votes and RCG/PHY-gate nodes, and verifies selector readback without
changing rate or gate state.

This sequencing follows the public OnePlus Android SM8450 kernel at
`449760504b3a75728eb60479f5b34dc51cb263ce`,
[`drivers/pci/controller/pci-msm.c`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/pci/controller/pci-msm.c).
Its source switch follows `pcie_phy_init()` and its power-off path restores
XO before disabling GDSC. Native ordinary-mux enable itself retains Linux's
selector-preserving behavior.

The host adapter test starts each simple mux at selector two, proves the first
AUX alias vote leaves selector two unchanged, checks selector zero inheritance,
and proves a second alias vote neither reprograms the mux nor adds another
native enable.

The October 4, 2026 firmware log (build time `10:12:58`) records PCIe1 AUX
`CBCR=0x80000220`, with the vote asserted, CLK_OFF set, FSM zero, and mux
selector zero. This is a real branch timeout before the QMP tables are
programmed. The log does not record the selector before the enable request,
so it cannot establish which parent the firmware inherited. Removing the
automatic source-zero request restores Linux's regular-mux enable semantics;
the consumer's explicit XO-to-PHY lifecycle addresses the cold source
dependency, but has not been validated on the board. The branch
retains `BRANCH_HALT_VOTED` polling and rollback. Host tests reproduce the
logged OFF/FSM-zero value and require timeout, vote rollback, and parent
reference release without a mux write; a separate FSM-ON case verifies the
Linux branch2 acceptance rule. A fresh board boot must establish PHY readiness,
PCIe link training, and endpoint enumeration.

## Portable target and Windows provider

The target graph is shared with ClockCr through Sm8450ClockLib; runtime nodes,
parent pointers and ownership counters are copied into private nonpaged state.
Create and mapping bind perform no MMIO, validate complete register windows and
publish no partial mappings on failure. Reset addresses come from the supplied
GCC mapping, and bit0 updates preserve other reset-register bits. Native reset
calls use the existing initialized target context and OSKAL lock/fence helpers.

The portable `ClockGdscSet` balances votes and restores inherited software
collapse/HW control; a failed final restore keeps ownership for retry. ClockCr
teardown drains all requests, releases leaf clocks before shared sources and
then releases power domains. Failed restoration keeps the mapping. Native and
NT sanitizer fixtures cover bounds, inherited votes, failed enable rollback,
failed release/retry and shared graph teardown. Windows adapter mocks separately
cover blocked-reader drain, partial mapping/allocation failures, invalid ACPI
resource preflight and retry without unmap. These checks and real ARM64 WDK
compile/link are not board or Windows execution evidence.

External RCG source/fractional and divider configuration now lives in ClockLib.
Native and Windows adapters call the same methods. Mapped addresses keep native
pointer width; configuration preflights every CMD/CFG/M/N/D register, preserves
reserved bits, and rejects active shared RCG owners. Failed updates restore the
previous register image; failed rollback relatch retains ReleasePending and
blocks enable/teardown until recovery succeeds. Mux readback failure restores the
old selector, and direct mux requests check repeated child owners as well.

Separate divider metadata describes the read-only DP link-interface divider,
fixed UTMI /1 contract and writable byte-interface divider. The byte-interface
branch retains the Linux byte0 RCG parent dependency. Divider-only RCG changes
survive the following cold enable. Native/NT tests exercise >4GB virtual
mappings, fractional/ integer transitions, conflicting owners, reserved bits,
read-only divisors and retained relatch retry; Windows adapter tests verify
revision2 method dispatch and request gates.

The same ClockLib now supplies an absolute per-owner voter in
`ClockVoteLib.h`/`clock_votes.c`. Repeated demands keep one owner reference;
removing that owner never consumes a foreign reference. Partial enable undo and
final release retain their exact parent/gate prefix. A rejected rate replacement
marks uncertainty when the old reference was already withdrawn, allowing the
journal to restore the old logical demand. Shared pending parent configuration
blocks a successful-looking idempotent enable. Preflight performs no MMIO and
bounds dependency traversal; exact supported rates are required.

ClockCr publishes a separate revision1 owner query interface for Validate/Get/Set
and ReleaseOwner, preserving the existing revision2 balanced interface. Owner
records use fixed nonpaged storage, preserve D0 sleep demands, and retire only
after the complete graph has restored both old-interface and owner votes. Every
request passes the same rundown and passive operation lock. The shared PEP
clock bridge snapshots only owner demands and accepts explicit board-qualified
rate tuples. Native/NT actual-core journal tests cover late write failure,
repeated resource rollback, retained domain/parent restoration and foreign votes;
the actual Windows bridge/adapter tests cover reference lifetime and D0/removal.
Permanent broker removal/rebind, full power policy and physical qualification
remain required before installation.

## Native retained acquisition contract

The native ClockCr protocol revision1.3 appends `AcquireClock` and `AcquireGdsc`. The returned `Owned` bit reports only the reference obtained by that call, including a failed acquisition whose hardware rollback retained cleanup. An error with `Owned=FALSE` must never be paired with a release of another owner's clock. An error with `Owned=TRUE` must retain prerequisites and retry `SetClock` or `SetGdsc` with `Enable=FALSE` until release succeeds; new acquisitions are declined while cleanup is pending. Callers keep their exact per-resource ownership bits through failed release.

`ClockAcquire` and `ClockGdscAcquire` measure the reference outcome while holding the shared OSKAL core lock. Native power domains use the reference-aware GDSC service and restore inherited collapse/HW-control state. The frontend stages rate intent, while the core checks ownership before programming the RCG. `ClockAcquireExternal` checks the leaf and mux for foreign or retained references before selecting a source, and performs selection and first acquire under the same lock. Explicit selector changes retain the existing separate single-owner configuration contract.

Actual native frontend/core fault regressions cover retained failed first acquire and final release, foreign RCG rates and pipe/mux selectors, failed foreign cleanup, inherited GDSC and lock/TPL boundaries. USB/DP/display/UFS/PCIe/GPI/I2C callers use the ownership result. GPI keeps live/touched/uncertain DMA clocks pinned; pre-hardware retries reuse their healthy prefix rather than accumulating votes. These tests establish software ownership behavior; hardware power and link readiness still require a board capture.


## Windows retirement of inherited display clocks

The revision2 external-clock interface appends `NativeRetire`. The caller must
hold the real native display handoff claim, its own D0 access and domain votes,
and fences for the route, PHY and bridge. The method validates the immutable
FINAL handoff record and the exact HDK8450 leaf, controller, rate and selector
inventory before writing hardware. It does not manufacture the native claim or
certify those other fences.

Seven private leaf gates retire before the five private source ROOT_EN votes.
Each withdrawal requires off readback. No synthetic ClockLib reference is
created to represent inherited hardware. Partial failure pins access and keeps
an exact owner/record journal; only that owner with the identical record can
retry. A completed retirement cannot replay against later Windows references.
Generic inactive reservations also exclude this transfer. Raw balanced clock,
source, divider and domain methods cannot remove required access or ancestor
votes while the external owner is active or recovering. D0 rundown rechecks
pending retirement after draining admitted operations and preserves retry
admission when necessary.

The native/NT core fixtures exercise all twelve halt failures and strict retry
identity. The actual permanent PEP provider fixture checks owner reservations,
access retention, legacy API exclusion and a request completing during D0
rundown. Standalone ClockCr and the combined PEP compile and link against the
ARM64 WDK. Full native route/PHY/bridge/dependency composition and physical
Windows validation remain outstanding.
