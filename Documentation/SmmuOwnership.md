# SMMU ownership and allocation

The Crane DXE driver uses the existing SMMUv2 controller without resetting its global state.
SM8450 calls `QcomMmuDetachHelperLib.MmuDetach()` in PrePi and preserves its
explicit MDSS SID keep list. The helper verifies stream retirement and global
TLB synchronization, then disables only eligible referenced banks. It preserves
stage-2, foreign, kept and Qualcomm final banks; failed handoff stops SM8450
before DXE. This early helper is distinct from the later per-domain DXE teardown
described below; mocked tests do not prove successful hardware release.
The caller must stop the device before attaching or detaching its DMA stream.
An existing standard IOMMU provider is not, by itself, evidence that a particular
stream has been configured; that requires an explicit platform ownership contract.
See [the SM8450 source audit](Sm8450SmmuAudit.md) for the actual detach call paths,
the early helper versus DXE per-domain detach, and the ExitBootServices failure limit.

## Linux and Android references

This allocation contract is based on public source:

- Linux revision `40288c9206c17eb66a603262e06a58d300d0f279`,
  `drivers/iommu/arm/arm-smmu/arm-smmu.c`: `arm_smmu_write_context_bank()`
  disables an unassigned bank by clearing SCTLR. Teardown releases the software
  allocation bitmap without clearing TTBR, TCR, MAIR, CBAR or CBA2R.
- OnePlus SM8450 Android kernel revision
  `449760504b3a75728eb60479f5b34dc51cb263ce`,
  [arm-smmu.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/iommu/arm/arm-smmu/arm-smmu.c):
  `arm_smmu_handoff_cbs()` preserves matched handoff streams and reserves the
  context banks they reference in the allocation bitmap.
- The same Android revision,
  [arm-smmu-qcom.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c):
  the Qualcomm handoff path reconstructs existing SMR/S2CR mappings and reserves
  their context banks. Qualcomm bypass-bank handling also motivates retaining
  the final bank rather than allocating it to a Crane domain.

No binary disassembly or private driver source is required for this contract.

## Allocation and preservation

`SmmuAttach()` rejects every valid exact or wildcard SMR matching the requested
SID, including existing bypass streams. It allocates an invalid SMR and a bank
that is outside the stage-2 and final bypass ranges, has SCTLR.M clear, and is
not referenced by any valid translating SMR/S2CR pair. Nonzero disabled-bank
configuration is not allocation state and must not cause false exhaustion.
ASIDs present in either TTBR of enabled or stream-bound banks remain reserved.

The selected bank's original stream and translation configuration is saved.
An empty translation table is published and its ASID invalidated before the
new valid stream match is installed. Mapping changes use break-before-make,
cache publication and TLB synchronization. Unmapped DMA is not granted access.

Detach first removes the stream match and completes invalidation, then disables
the bank, restores the saved configuration and invalid stream state, and restores
the original disabled SCTLR value. Format/TCR controls precede TTBR restoration;
short-descriptor contexts use their original 32-bit accesses. Required writes
are verified before proceeding. Failed programming retains domain ownership and
tables; a TLB timeout additionally stops further commands to the controller.
The adapter serializes these operations and requires device quiescence before
resource release. Retention during boot does not reserve arbitrary caller pages
against OS reclamation after a failed ExitBootServices fence.

SM8450 supplies MDSS ACTLR=1 for SID/mask 0x2800/0x402 from public Android DT.
Only matching, newly owned banks are changed, verified and restored. The driver
does not unlock secure/global ACTLR protection or modify foreign contexts.

This preserves observable hardware ownership. A separate firmware component
must not concurrently claim the same disabled, unreferenced bank without a
shared ownership contract. There is no fallback to global reset or bypass.

## Boot evidence and validation

The HDK8450 log reports ID0 `0x4C017E71`, ID1 `0x6000004B`, and ID2 `0x00005111`:
113 stream matching entries and 75 context banks. PCIe SIDs `0x1C00`/`0x1C80`
and QUP SID `0x56` failed bank allocation under the previous all-zero-register
predicate. This log does not contain the actual bank registers, so it does not
prove stale configuration was the only source of exhaustion.

The host regression uses that geometry with nonzero stale configuration in
all banks. It checks reuse and detach restoration while preserving enabled,
stream-bound and reserved banks, their ASIDs, and unrelated streams. It also
checks genuine stream/bank exhaustion. The old predicate fails this fixture.
These checks do not prove board DMA or device enumeration.

Boot diagnostics use DEBUG_ERROR for visibility with the existing DEBUG image
mask. They record probe and allocation stages, SID/SMR/CB/ASID assignments,
register readback failures, TLB synchronization and mapping outcomes. A
bank-allocation failure reports bank configuration and exclusion reasons.
The level carries diagnostic progress as well as errors. LT9611 FIFO/GPI/FIFO
success, actual display output, and UFS/PCIe enumeration still require a new
hardware boot.
