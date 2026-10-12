# CRDK source organization

CranePkg owns the shared register, transport and policy cores. `CrDK.json` lists
the Windows compile units, property sheets, public headers and source-level
dependencies. WDF/UEFI adapters own device discovery, translated mappings, DMA,
firmware files, operating-system notifications and final resource lifetime.

Validate the catalogue and synchronize a Windows checkout:

```sh
python3 Platforms/CranePkg/Tools/export_crdk.py
python3 Platforms/CranePkg/Tools/export_crdk.py --destination ../CrWDDK/CrDK --write --adopt-existing
python3 Platforms/CranePkg/Tools/export_crdk.py --destination ../CrWDDK/CrDK
```

Use `--adopt-existing` only for the first managed export into a legacy checkout.
Every replaced file is backed up under its parent's `Build/crdk-export-backups`.
Later exports reject destination edits that differ from the previous manifest.
The exporter preserves `.git` and all unmanaged files, including legacy native
copies. Only the catalogue determines active SDK sources; those legacy copies
are not Windows build inputs. `crdk-export.json` records exact input hashes.

Import `CrDK.props`, then the property sheets for the required libraries and
their dependencies. Property sheets do not silently import other compile units:
each dependency is imported once by the consumer project. Include the project's
WPP `trace.h` and configure the OSKAL `log_*` functions as documented in
`Include/oskal/cr_debug.h`. Compile-only checks that substitute WPP tracing do
not validate trace generation or Windows package creation.

`OskInterruptLib/oskinterrupt_wdf.c` is the Windows IRQ backend; its native
counterpart remains in CranePkg. GPIO/PDC native IRQ convenience wrappers are
not Windows compile units. Windows adapters supply translated interrupts and
choose DPC/work-item execution via OSKAL; they do not reuse firmware GIC numbers.

`ClockController.Address` is an adapter-owned mapped virtual address on Windows.
Set `MemMapped` after successfully mapping the translated `_CRS` window. Kernel
`ClockLibInit` rejects missing mappings; `ClockDeinit` does not unmap memory owned
by the adapter. Firmware continues to use the GCD mapping implementation.

The SMEM, UCSI, SPMI, PMIC GPIO, button, interconnect, SMMU, PCIe, PIL and MDSS
cores are exportable independently of native protocols. PIL's shared callbacks
have a separate native-only PilUefi bridge; MDSS's native scanout uses the
portable fetch-start helper. Native drivers retain their firmware discovery
and lifecycle adapters.
The catalogue is a source/export contract, not a claim that a Windows adapter or
its ACPI, power sequencing, signing or hardware validation is complete.

`PepCrLib` consumes an adapter-decoded immutable LPMD tree. It validates device
and component D/F/P states, abandon states and the original Kodiak CLOCK,
FOOTSWITCH, PMICVREGVOTE, NPARESOURCE and BUSARB operations. Resource providers
receive raw opcodes, units and full-width votes. Every transaction preflights
all operations, journals changes through provider callbacks, and returns both
operation and rollback errors. A failed Begin must leave the owner unchanged.
The caller retains any owner whose rollback failed and serializes transactions.
This policy core does not register Windows PoFx callbacks or implement providers.
Device/component descriptions expose exact masks and policy-owned ID views.
Complete-device preflight rejects unsupported future resource operations before
prepare writes. A batch applies up to65 ordered D/F/P transitions for one owner
with one Begin/Commit/Rollback, covering D0 plus F0/P0 for32 components.
Windows `~/CrWDDK/Common/CrPepDpm` adds permanent PoFx registration and passive
worker dispatch. `PepJournal.c` now owns portable per-operation undo records
and committed per-owner baselines. It records before attempted writes, reverses
repeated-resource operations in order and retains failed rollback/close entries
until retry. Snapshots describe only the logical owner vote; qualified providers
must recompute shared aggregates and preserve other owners. Caller-owned storage
and provider identities persist through recovery. Windows `Common/CrPepResources`
adds permanent nonpaged owner storage, complete-policy capacity/preflight checks
and DPM wiring. Qualified hardware backends and installable PEP/bus integration
remain unfinished. See `~/CrWDDK/docs/PepRuntime.md` for exact lifecycle scope.

`Tests/run_pep_tests.py` reads the original Kodiak DSDT from this workspace and
exercises all five devices/239 resource operations with native and NT status
semantics under sanitizers. `Tests/run_clock_ownership_tests.py` checks mapped-clock/GDSC ownership and
shared graph teardown; `Tests/run_resource_core_kernel_tests.py` checks PDC
boundary behavior. `Tests/run_crdk_export_tests.py`
checks that SDK updates preserve Git metadata, backups and local modifications.

GpioConfigLib consumes the GPIO ACPI schema through validated portable trees. It checks the full table before publishing configuration, including bit widths, unique pin/PDC mappings, defined tile/register references and translated TLMM limits. Special RCFG entries describe a TLMM control-bank offset plus bank-relative subregister offsets, normalized to the GPIO core representation. Its regression uses the Windows SM8450 tables, including all 214 entries (210 regular GPIOs and four special pads).

Windows resource adapters now use the shared bounded no-argument ACPI evaluator.
The decoder accepts immutable integer/string/buffer/package views; wire storage
and nodes have explicit WDF ownership. RPMh requires PepCrLib for GCFG buffer
decoding and declares its command-DB ACPI dependency. Requests are admitted only
in D0, remote-provider callbacks drain readers before dereferencing, and OSKAL
IRQ detachment precedes MMIO unmap. A provider that is permanently removed
requires its dependent device to be prepared again before name-based requests.

Actual ARM64 WDK compile/link checking resolves project property sheets and
compiles all imported sources through `~/CrWDDK/tools/check_resource_wdk.py`.
WPP is substituted and outputs are unsigned; this does not verify INF/signing,
MSBuild packaging or Windows execution. Shared evaluation failure/retry/lifetime
and actual RPMh adapter concurrent removal are covered by sanitizer tests in
`~/CrWDDK/tests/run_acpi_tests.py` and `run_rpmh_adapter_tests.py`; RPMh hardware
completion/configuration remain covered by native core regressions.

`Sm8450ClockLib` now exports the native GCC/DISPCC target graph. Firmware uses
its existing single context; Windows creates a private graph in device-owned
nonpaged storage. Mapping bind preflights all seven windows, and reset operations
use the mapped GCC address while preserving unrelated bits. `ClockGdscSet`
serializes reference counts, restores inherited collapse/HW control on the final
release and retains failed restoration for retry. ClockCr GCFG revision1 identifies
SM8450 and the `_CRS` descriptor-to-controller map; it includes the Linux GCC
window at 0x00100000 with size0x1f4200. Unsupported inventory entries stay absent.
Branch clocks retain an acquired parent prefix and a separate gate owner.
If release fails, retry visits only dependencies still owned by that branch;
it cannot decrement a successfully released parent's foreign reference twice.
Re-enable is rejected until cleanup succeeds. Failed cold-enable rollback keeps
its cleanup reference, including a failed parent acquisition that retained one.
Native/NT ownership regressions exercise each path against the actual core.
`ClockVoteLib.h`/`clock_votes.c` adds allocation-free absolute votes in the same
library. Each stable owner has its own logical demand and retained cleanup
reference; replay does not add another enable reference. Pure preflight bounds
the dependency walk and requires exact RCG table rates. Conflicting rates remain
an explicit failure, without guessing an aggregation rule. All direct graph
operations and voter operations require the same outer serialization.
`Reserve` claims every stable owner/node before permanent publication, without
IO; `RequireReservation` checks that capacity proof. Restoring absence retains
the slot until explicit owner release or drain. The Windows owner interface is
revision2 and requires reserved Get/Set; the separate balanced ABI is unchanged.

`PepClock.c` connects qualified CLOCK/FOOTSWITCH programs to that owner API.
Bindings explicitly associate policy names with hardware/controller names and
board-qualified value/selector/Hz tuples. Duplicate aliases and unknown modes
fail before votes. Journals snapshot logical owner values and preserve other
owners through failed writes, rollback and Close. The Windows
`Common/CrPepClock` adapter references ClockCr's separate owner query interface;
the balanced revision2 interface remains available. Kodiak USB's omitted
FOOTSWITCH mode needs an explicit AllowImplicitGdscMode binding; display's
explicit mode is preserved by the parser. The bridge accepts only whole-domain
mode1. Pure `Describe` shares exact opcode/tuple decoding with pre-publication
reservation helpers without calling provider IO. Permanent broker rebind, NPA qualification and consumer readiness remain.

`~/CrWDDK/ClockCr` exposes a revision2 kernel clock/GDSC/reset/source/divider query interface. D0 request
rundown precedes release, graph votes are restored before unmap, and failed
restoration retains the state/mappings. `run_clock_adapter_tests.py` compiles the
actual Device/Interface sources with adversarial WDF lifetime/core mocks; core
hardware sequencing has separate native/NT sanitizer coverage. The shared source/divider methods validate mapped spans and fractional widths,
preserve reserved bits, reject conflicting owners and retain failed RCG relatch
for recovery before unmap. System-sleep retention and PEP/DOD integration remain
to be completed and validated. Compile/link success does not establish Windows runtime behavior.

`Sm8450InterconnectLib` exports the immutable Linux-derived topology through a
const getter; native DAL retains its compatibility getter. Each Windows owner
must supply nonpaged private voter storage and serialized init/lifetime changes.
The voter uses OSKAL `CR_MUTEX` so callbacks remain at PASSIVE_LEVEL; elevated
Windows IRQL is rejected before mutation. Full VCD groups must fit the provider's
TCS capacity, checked before the first vote. A failed RPMh batch is uncertain,
so every BCM in it must be restored. Failed transaction rollback retains logical
paths and gates mutation until recovery; failed init/teardown retains provider
and callback ownership until `InterconnectLibDeinit` succeeds. The native adapter
keeps an unpublished image resident when cleanup fails and rejects unload with
active path handles. Host native/NT voter and actual DXE failure regressions are
in `Tests/run_icb_tests.py`.

Windows `RpmhCr` now owns the private voter alongside its TCS, interrupt and MMIO
resources and publishes the revision1 interconnect query interface. RPM0's ICFG
package identifies schema1/SM8450. Initialization holds one command-DB reader
lease across metadata loading; subsequent votes use cached metadata. D0 request
rundown drains transactions while retaining logical path handles across sleep.
Release clears BCM votes before retiring the RPMh owner; failed clear retains
the mappings, IRQ and provider references for retry. The actual adapter and
shared-voter sanitizer suite covers these paths, including concurrent removal.
All12 RPMh project units compile/link against real ARM64 WDK, and ACPICA checks
the actual ICFG method. Permanent-removal recovery through the PEP/bus journal
and complete system-sleep policy remain pending; these checks do not establish
Windows runtime behavior.

`InterconnectVoteLib.h`/`icb_votes.c` reserves private core paths for stable
owners and stores absolute Linux kB/s demands. Reserve the complete owner/path
set before publishing a permanent PEP: the core has32 handles shared with
legacy consumers. Journal Resolve never acquires a handle or repairs hardware.
Absent demand retains a zero-demand reservation, so successful Close permits
future pure qualification. `InterconnectValidatePath` verifies provider-local
endpoints/connectivity; `InterconnectGetPathVote` copies logical demand plus
global rollback uncertainty. Both perform no RPMh/command-DB IO. The bank's
OSKAL mutex nests before the existing core mutex; normal legacy handles stay
independent. Failed release retains its entry. After all requests drain and
core Deinit succeeds, the Windows adapter retires invalid owner handles before
unmapping; a failed clear retains the bank and hardware for retry.

`PepBandwidth.c` maps exact qualified BUSARB master/slave pairs to target paths.
Bindings declare AML units:1 for Linux kB/s or1000 for bytes/sec. The latter
rounds fractional demands up as an explicit minimum-demand policy; this is
separate from Linux's truncating Bps_to_icc macro. Linux's base1000 units and
sum-average/max-peak aggregation are documented in
[interconnect.h](https://raw.githubusercontent.com/torvalds/linux/master/include/linux/interconnect.h)
and [icc-rpmh.c](https://raw.githubusercontent.com/torvalds/linux/master/drivers/interconnect/qcom/icc-rpmh.c).
`RpmhCr` publishes a separate owner query GUID alongside revision1; Windows
`Common/CrPepBandwidth` holds an interface reference and forwards journal IO.
Native/NT actual-core and actual Windows adapter sanitizers cover reserved
capacity, units, competing owners, partial VCD writes, failed restore/Close,
D0 retention and failed-clear teardown. These provide no Windows installation
or hardware validation. Permanent removal/rebind, qualified rails and complete
bus/consumer readiness handoff remain pending.


`RpmhRailLib.h` extracts the native regulator supply graph into RpmhLib; the
actual RPMh DXE now forwards acquire/release and uncertain write tracking to
that core. Fixed voltage/mode contracts share parent leases, record requests
before uncertain hardware writes and retain only their own failed cleanup.
`RpmhRailVoteLib.h` adds absolute fixed-storage owner votes and explicit pure
reservations. Optional unresolved parent chains decline pure qualification;
lazy address discovery remains a native graph operation outside qualification.
All raw, graph and bank operations require the same outer serialization.

`PepRail.c` preserves Kodiak USB's six-field and display's eight-field PMIC
packages. Every complete five-integer tuple maps through an explicit qualified
profile, including driver/vote strings; no selector, units or mode is guessed.
Pure `Describe` is shared by qualification and reservation setup. Capture and
Restore use only this owner's logical demand. An unchanged disabled or absent
owner can restore its vote while a foreign owner retains uncertain hardware;
Get still reports that uncertainty and gates consumer access. Native/NT actual
core+journal vectors cover conflicts, shared parents, committed errors, failed
Close and reservation retention. Windows rail binding is implemented below; NPA and permanent recovery remain pending.

Windows PIO0 now exposes direct PDC1 as virtual interrupt pin384, using a typed
route and explicit GDPI whitelist. The shared loader rejects aliases, range
holes and independently locked routes sharing an enable or SPI RMW word.
Actual GpioClx callbacks reject invalid banks before locks and never touch TLMM
for the direct route. SPM0 consumes that GpioInt through SpmiCr's shared OSKAL
WDF DPC backend. Separate request/dispatch rundown protects callback retirement;
callback bus IO accepts DISPATCH_LEVEL while IRQ mutations require PASSIVE_LEVEL.
A failed release retains ownership for retry and clears the prepared flag.
Actual adapter sanitizers, GPIO13/SPMI17 ARM64 WDK project checks and ACPICA
route/configuration checks pass. These do not validate runtime wake or permanent
surprise removal; the PEP/bus journal and full sleep policy remain pending.

Windows `SmemCr` (SME0/ALCR000B) owns the2MiB SMEM region and only the4-byte
TCSR mutex3 register. Its revision1 query interface has explicit BeginAccess/
EndAccess leases. Keep a lease until every use of the returned pointer ends;
D0 exit/removal drains leases before unmapping. Allocation remains PASSIVE and
serializes locally with a WDF wait lock, plus the shared `smem_mutex.c` remote
mutex. An uncertain unlock retains ownership; a later allocation first retries
release, never clears a foreign owner, and cleanup retains both mappings on
failure. D0 metadata is revalidated before access resumes. Native DXE uses the
same mutex core and release recovery; its legacy get-or-allocate contract is
preserved. Actual native/NT core, native DXE/SEC and Windows adapter sanitizer
vectors pass, as do all9 SmemCr project WDK units. Permanent removal and full
remote system-sleep coordination remain part of the PEP/bus work.


## Qualified rail and consumer interfaces

`Sm8450RailLib` initializes a private 14-resource HDK supply graph. It performs
no IO or allocation. `RpmhCr/Rails.c` caches the graph's command-DB addresses
under one provider lease and requires the board-qualified VCFG `{1,0x8450,1}`.
Only canonical accelerator4, 16-byte VRM resource bases qualify. Missing optional
metadata gates that supply chain. The separate revision1 rail-owner interface
reserves stable owner/resource capacity before Get/Set, takes exact mV/hardware
mode values and retains failed cleanup. Raw compatibility writes cannot bypass
any rail or parent in this qualified inventory. `Common/CrPepRail` connects
explicit policy profiles to this interface; no value/selector conversion is
inferred. Shared helpers validate complete address spans, 13-bit voltage fields,
3-bit modes and overlapping supply tuples before writes.

`PILCrLib` separates portable PAS/ELF logic from native protocols. Native
`PilUefi.c` clears the complete secure-call frame. Windows exports additionally
include the qualified ARM64 SMCCC helper and its clang/MSVC assembler dialects.
The catalogue's optional `assembly_sources` records both; exporters validate the
MSBuild CustomBuild input without executing its command. Linux ARM64 checks
assemble only the declared clang source, link its real COFF object and hash both
dialects. They do not verify armasm64 or MSBuild execution. The helper does not
establish a firmware or hypervisor secure-call contract on a Windows machine.

`MdssDpuInterruptCrLib` exports the portable SM8450 WB2/vblank event ledger and
read-only shared MDSS source qualifier. Its native INF and Windows property
sheet compile the same two sources. The separate private Windows OSKAL adapter
uses actual interrupt synchronization and a counted IO lifetime. Library export
does not connect the physical IRQ: the display Root must still supply and retain
the six assigned views, power, retired foreign writers and complete callback
drain. The qualifier's pinned Android/mainline inventory and tests are described
in [MdssInterruptSources.md](MdssInterruptSources.md).

Windows `PilCr`, `UcsiCr` and `MdssCr` consume these cores. PIL keeps physical
metadata separate from virtual parsing buffers and retains failed PAS shutdown.
UCSI uses OSKAL IRQ261 and the raw PMIC PPM transport with UcmUcsiCx as the sole
OPM. DOD supplies fixed SM8450 DPU/DSI/LT9611 presentation and retained scanout
ownership; DP is a separate hardware path. ACPI PIL0/UCS0/MDCR remain default-off
until qualified platform backends and ownership handoff exist. Frontend/core
compile and sanitizer evidence does not imply installable packages or hardware
readiness. See the Windows driver ReadMe/README files and `docs/ResourceProviders.md`.

## Audio foundations

`AcdbCrLib` validates AudioReach type0 ACDB framing and HEADv1 metadata and returns bounded chunk/codec/OEM offsets. `AudioGprCrLib` decodes GPR and builds/matches explicit SPF readiness messages. Both are stateless, allocation-free libraries with retained upstream attribution; neither currently programs a codec or publishes an audio endpoint. Real upstream calibration fixtures are test inputs, not HDK8450 calibration.

`UcsiCrLib` also supports independently registered named GLINK services on its existing single SMEM/IPCC edge. Caller-retained buffers provide up to32 receive intents and64KiB messages. The externally serialized IRQ/TX pump handles both open directions, fragmentation, channel generations and close acknowledgments. A callback must copy its packet and return exact success; other results close only that service. Timeouts do not release buffers, and a local close acknowledgment alone cannot retire a delayed peer open. OS adapters must share the physical edge and drain their own callbacks before releasing client storage. The existing Windows PMIC-exclusive transport interface has not yet been replaced with that shared client provider.

`AdrenoPowerCrLib` constructs the A730 mainline-family HFI performance records
from immutable qualified ARC snapshots and an explicit OPP profile. It retains
source corner indices, the GX/CX wire order and optional GMXC semantics, and
rejects malformed or overlapping data before publishing its pointer-free result.
The exported SM8450 SoC profile is reference data; the consumer must qualify
board speed bins, actual firmware and resource ownership before selecting rates
or submitting HFI. This library performs no hardware access and does not start
GMU, program power votes or establish a live GPU context. Its GPL-2.0-only
license and source provenance accompany the export.

`AdrenoHfiCrLib` supplies the A730 shared-memory queues and the four-command
PERF/BW/CORE_FW_START/START boot exchange. It checks exact ACKs, bounded ring
indices, publication ordering and empty-queue notification races, and retains
failed transactions without replay. Native users provide `SynchronizationLib`;
Windows users provide the actual power, memory, translation and DMA visibility
callbacks through the execution owner. The library uses AdrenoPower types but
does not call its implementation. Boot completion covers these four ACKs;
runtime commands and interrupt-driven asynchronous servicing remain separate
work. The export includes its GPL-2.0-only license and Linux source provenance.

`AudioPcmCrLib` exports one-region APM MAP/UNMAP and metadata-free V2 READ/WRITE
packet builders with exact completion matching, using `AudioGprCrLib`. The
portable core validates addresses and wire fields; the consumer still owns the
real DMA grant, cache operations, endpoint and firmware capability checks.
Export does not enable PCM transmission or create an audio endpoint. Its
BSD-3-Clause attribution and pinned AudioReach references accompany the library.

`MdssDpCrLib` exports the checked SM8450 AUX FIFO/EDID transport with retained
failure cleanup. The caller must provide the actual display and shared PHY
leases and serialize requests. It does not acquire mainlink, framebuffer or DPU
resources, and it is not yet selected by the native DP driver or Windows DOD.
The source retains GPL-2.0-only attribution. Windows integration must also retain
the sole MDSS IRQ owner and exclude the shared DP/WB2 hardware slice.

`AdrenoStopCrLib` provides a checked A730 reset/AXI stop sequence and fresh
register observations under a real retained owner. It preserves the Android
ten-TCS policy and client-then-arbiter halt order. Its owner must first drain
software and IRQ writers; successful checks do not authorize SMMU detach,
memory release, power collapse or a live GPU context. Native users supply
SynchronizationLib; both backends use Sm8450GpuResourcesCrLib.

`ScmCrLib` provides typed PAS1 and GPU secure-operation ownership, serialized
continuations, retained uncertain replies and permanent quarantine. Its private
PAS translation unit preserves the existing MIT helper and uses private symbols.
SDK registration does not select a live conduit. A host must supply actual
resource/allocation authority and migrate every direct PAS caller together;
the independently unloadable frontends cannot own the permanent transport.

## Audio Graph/Pull source integration (2026-10-09)

`AudioPullCrLib` is now registered with Graph/PCM/GPR dependencies and exported
to the managed Windows SDK, including its public header, README and full
LICENSE. The catalog currently contains73libraries and420export files.
The PEP owns the permanent Graph/Pull namespace and authenticated source
fence; Windows AudioCr owns profile import and native consumer lifetime.
The actual current PEP164-C/two-assembly project and AudioCr15-C project
linked successfully against ARM64 WDK types. The complete SDK127-C/one-assembly
probe also linked using the project's Clang23 toolchain. WPP is substituted,
and these outputs are unsigned cross-build checks.

Portable packet/ownership regressions are maintained in the source tree:

```sh
python3 Platforms/CranePkg/Tests/run_audio_pull_tests.py --output .tmp/audio-pull-tests
```

The labeled synthetic ACDB/request and controlled GLINK/remote peer exercise
actual Graph/Pull/PCM/Cal/GPR source in EFI and NT host modes. Separate recorded
native source compositions test real PEP/PIL-origin admission, mandatory Ready
position reads, reader joins, graph CLOSE and both exact UNMAP acknowledgments.
The combined current source tests passed62host runs. Root evidence is
`.tmp/waipio-continuation/runtime95-root/artifacts.json` (`71f38e3a`).
Only the exported README changed after project compilation; every compiled
source/header byte remains identical and the tested export snapshot is retained.

Native DMA/Pull/View/IORT files are compiled source checkpoints and remain
unselected by the actual15-source AudioCr project. No HDK8450 firmware/profile
is bundled; installed SID1801, analog WCD/WSA/SoundWire route, a WaveRT endpoint,
audible playback and abnormal physical removal retirement remain unfinished.
Source loss or a new ADSP generation cannot retire old remote DMA. Runtime
position observations are raw counters/index/timestamps and do not grant safe
user writes, wrap reconstruction or latency.

The shared IPCC RX/SMP2P Subsystem revision has independently passed its frozen
source review. Its cumulative Audio merge is still private: Root's43-path
preparation preserves all Audio callbacks and existing catalog entries, and
its166-C/two-assembly PEP link passed. Selected shared-RX/Monitor/PIL plus Audio
lifetime proof and final actual-current checks precede publication.
