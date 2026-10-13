# AdrenoStopCrLib

This portable SM8450/A730 sequencer holds the GPU/GMU in reset with a checked
AXI fence while its real owner retains power, mappings and dependencies. It
provides a fresh register observation. It does not establish full power-off,
HAL domain attachment, secure aperture ownership or permission to free memory.

The implementation and source-derived register table are GPL-2.0-only; see
`LICENSE`. The native library uses `SynchronizationLib` and the actual
`Sm8450GpuResourcesCrLib` resolver. Windows builds use the OSKAL interlocked
operations and the same resolver. No allocation, recursion, mapping, SMC or
SMMU register access occurs in this library.

## Owner and API contract

`CrAdrenoStopInit` binds an initially zeroed, independent, naturally aligned,
resident context to its exact immutable IO descriptor and nonzero Start
incarnation. The context, descriptor and complete backend storage range must
be separate. A copied context cannot operate. Initialization and retirement
are externally serialized; normal operations run at PASSIVE level.

The backend must validate the real PDO/Start incarnation, assignment, retained
power and mapping leases inside every typed register access, barrier and
delay callback. It resolves register IDs with `CrAdrenoSm8450StopResolve`,
checks access flags and retains its actual mapping through completion. A
successful descriptor lookup alone does not authorize MMIO. In particular,
the backend must reject GPU-bank accesses if the source-qualified GX branch
is no longer accessible.

Before `CrAdrenoStopFence`, the owner closes and drains CPU, HFI and ring
writers, and its actual interrupt, DPC and work callbacks. The current
GpuCr diagnostic observer does not supply these prerequisites. A caller
Boolean cannot replace them. The owner must also prevent another power or
reset actor from changing the observed hardware while consuming the result.

| API | Behavior |
| --- | --- |
| `CrAdrenoStopFence` | Execute the fixed sequence, retain every attempted mutation, and stop at the first nonexact status or failed observation. |
| `CrAdrenoStopCheck` | Re-read the final conditions under the same backend and generation. On failure leave output unchanged and invalidate held evidence. |
| `CrAdrenoStopInvalidate` | Invalidate evidence before an actual owner resume/reset/domain transition; permit the next run to qualify its new GX branch. Preserve the mutation journal. |
| `CrAdrenoStopSeal` | Atomically prohibit new work without MMIO. `CR_BUSY` means the seal committed while an admitted operation still needs to drain. |

The owner retains the context, descriptor, backend and callback code after a
busy seal, drains the actual admitted call, then retries seal. A callback
already admitted can finish; a later step observes closure before continuing.
Seal never releases physical resources. `Check` publication linearizes at its
last live check; a subsequent seal or hardware transition makes that historical
observation insufficient for further operations.

Positive warning/nonfinal callback statuses become `CR_DEVICE_ERROR`; the raw
first callback status remains available in `FirstRawStatus`. Locally detected
failures are in `FirstFailure` and `FirstFailurePhase`. Attempted-write bits are
set before callbacks, acknowledged bits only after exact success. These bits
describe possible past effects, not current hardware state or a count of writes.

## Fixed inherited Android policy

The only supported policy is `CR_ADRENO_STOP_POLICY_SM8450_ANDROID`. Its GX
observation uses raw status bits 0 and 1: both clear means on. A retry must keep
the same observed branch until an explicit owner invalidation.

1. Mask accessible GPU interrupts and both GMU interrupt sources. Clear GMU
   keepalive and retain A7xx GEMNoC Qactive bit 0.
2. Require all ten Android Gen7 RSCC TCS entries idle, each bounded by 1 ms,
   then sequence-busy zero bounded by 10 ms. There is no fallback to four TCS.
3. Clear AHB dropped-write flags with the W1C command 7, allow AHB access and
   assert CM3 reset. Ordinary control writes have ordered readback.
4. If GX is on, request its GBIF halt and require its ACK. Drain the CX client
   phase with control 1, then request and **retain ARB control 2**. ACK waits
   are bounded by 100 ms. An inherited or previously attempted ARB2 is never
   weakened back to CLIENT1 on retry. Unknown control values are unsupported.
5. If GX is on, assert GPU software reset and hold it for at least 100 us.
   Fresh checks require CM3 reset, ARB2 control/ACK, masks, Qactive, AHB state,
   the qualified GX controls and all ten idle TCS. CX busy is recorded raw;
   it is not interpreted as proof of graceful firmware slumber.

The final control is source-qualified ARB2, never an invented CLIENT|ARB mask 3.
GMU/AO writes use all ones; readback requires their defined source interrupt
bits rather than assuming reserved bits read back as ones. The AHB W1C command
is never treated as a restorable register image.

This sequence deliberately excludes halt/reset release, RSCC sleep, regulator
collapse, GPUCC ownership, SMMU stall-on-fault resume, PAS shutdown, HFI packets
and firmware/queue execution. Failures retain the dependency and mutation
obligations. Later real provider composition must qualify safe recovery.

## Pinned source evidence

The test runner checks these local source revisions and extracts an independent
register-address oracle from Android `gen7_reg.h`:

- Mainline Linux `a53261b60834ce617cc4f3c7723dda8285c59789`,
  `drivers/gpu/drm/msm/adreno/a6xx_gmu.c`: `a7xx_gmu_gx_is_on`,
  `a6xx_gmu_force_off`, `a6xx_rpmh_off` and `a6xx_rpmh_stop`;
  `a6xx_gpu.c`: `a6xx_bus_clear_pending_transactions` and GPU reset order;
  `registers/adreno/a6xx_gmu.xml`: register origins and interrupt fields.
- Qualcomm Android Waipio kernel
  `e93f69dc83e69e5139e6a7911041f357b5e2b304`,
  `drivers/gpu/msm/adreno_gen7_gmu.c`: `gen7_complete_rpmh_votes`,
  `gen7_gmu_pwrctrl_suspend` and `_do_gbif_halt`; `gen7_reg.h`,
  `adreno_hfi.h` and `kgsl_gmu_core.h`: addresses and interrupt fields.

Mainline A730 polls four TCS and clears the final bus halt; Android Gen7 polls
ten and retains ARB2 before attempting collapse. This explicit inherited
Android policy takes the latter conditions and retains power. It uses
mainline's checked client/drain phase only when ARB2 is not already asserted.
Mainline's RSCC sleep depends on its exact firmware-start history; inherited
firmware does not establish that history. Android's SMMU fault-resume action
cannot be applied to a HAL-owned domain by this sequencer.

## Verification and remaining integration

`Tests/run_adreno_stop_tests.py` compiles the actual engine, target and resource
resolver. Native and NT DEBUG/RELEASE sanitizer modes cover on/off branches,
all ten TCS, status failures and seal at every callback, uncertain writes and
retry, dropped controls, stale ACK, bounded timeouts, fresh-check losses,
address/epoch/alias misuse, and a real paused-thread seal/drain race. Its
receipt freezes actual compiler dependencies, source references, logs and
objects; source archives are tar files to avoid firmware metadata discovery.
Optional real EFI and WDK ARM64 compilation emits stack-usage files.

These tests model the IO backend. They do not prove a Windows power/IRQ lease,
actual GPU DMA retirement or physical A730 behavior. The next consumer must
compose the real writer/IRQ drain and dependency ownership, route all accesses
through the retained six resource views, and test failure retention through
its actual remove, D-state and Start-incarnation transitions. No existing DOD
driver, firmware image or live hardware path is enabled by this library alone.
