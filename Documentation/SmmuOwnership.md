# Waipio SMMU ownership

This note defines the boundary between the Qualcomm `HALIOMMUDxe` binary and
`SmmuCrDxe` on the two SM8450 Waipio targets. The conclusions below come from
static analysis of the binaries currently packaged in this tree. They are not
runtime proof from either board.

## Audited binaries

| Target | SHA-256 | Private protocol GUID RVA | Protocol RVA |
| --- | --- | --- | --- |
| `qcom-hdk8450` | `e3419b374821c762dc8a95ab3615d9feff02157dddbe24f0f885438f197db7ae` | `0xe078` | `0xe0c8` |
| `qcom-mtp8450` | `175149cda90137ab19686b6ef36d2de4ca3d0021e5cbe4fe7f262c315346ce45` | `0xe068` | `0xe0b8` |

Both files are AArch64 PE32+ EFI boot-service drivers with a PE entry RVA of
`0x1000`. Neither contains the EDKII IOMMU protocol GUID
`4e939de9-d948-4b0f-88ed-e6e1ce517c1e`. Both contain Qualcomm's private
protocol GUID `54b6d3b4-5d33-4f91-8600-6c41d5deb19a` and publish revision
`0x00010002`.

The private protocol's init callback at `0x16e8` returns a function table. In
the HDK image that table starts at `0xe0e0`; the MTP data is shifted by 16 bytes
but contains the same code RVAs. The relevant callbacks are:

| Operation | RVA |
| --- | --- |
| domain create / delete | `0x1928` / `0x19d0` |
| attach / detach named component | `0x1b40` / `0x2858` |
| configure domain | `0x2d98` |
| configure bypass domain | `0x3028` |
| map / unmap | `0x4408` / `0x4678` |

The semantic driver entry at `0x1738` installs the private protocol and then
calls the IORT parser at `0x6f64` in the HDK image or `0x6f54` in the MTP image.
The entry path does not call the SMMU register-access helpers and does not reset
the global SMMU, allocate a context bank, or install an SMR. Hardware changes
occur only when a client invokes the private function table.

## Context-bank behavior

The context-bank allocator is the byte-identical routine at
`0x2178..0x22e8`. It reads every CBAR and accepts a bank when its type is 0 or
2, or when its type is 1 and VMID is `0xff`. It then programs type 1, VMID 0,
memory attributes `0xf`, and the requested `CBA2R.VA64` value.

Crane programs `CBAR=0x0001f300`. This is type 1 with a VMID other than
`0xff`, so the audited HAL allocator does not regard a Crane bank as free.
HAL domain deletion releases only context banks recorded in that HAL domain's
private object list; its release helper at `0x1ab8` writes type 1 with VMID
`0xff`. No path was found which sweeps or resets every context bank.

`SmmuLib` applies a more conservative rule in the other direction. It rejects
a bank with a live SCTLR or residual CBAR, CBA2R, TTBR, TCR, or MAIR state and
therefore preserves banks configured before `SmmuCrDxe` starts. The two
allocators can consequently use distinct context banks if no third component
resets the controller.

## Stream-map behavior

The HAL SMR allocator at `0x24f8..0x261c` scans all but the final SMR and uses
the first entry whose valid bit is clear. It does not overwrite a valid Crane
entry. It also does not check whether another valid entry already matches the
same SID or an overlapping masked SID. A HAL attach for a SID already owned by
Crane can therefore create a duplicate match with undefined selection between
the two S2CRs.

The detach helper at `0x2b98..0x2cb8` is a stronger conflict. It scans from
entry zero and clears the first SMR whose raw value equals `VALID | SID` (and
the associated S2CR). It does not verify that the S2CR points to the context
bank owned by the calling HAL domain. If HAL and Crane contain duplicate exact
matches, a HAL detach can clear Crane's entry.

Crane avoids the reverse collision: `SmmuAttach()` rejects any existing valid
SMR which exactly or mask-matches the requested SID, selects only an invalid
SMR, and records/restores the registers it changes. This protects mappings
that existed before Crane attached, but it cannot prevent a later private-HAL
client from creating or detaching a duplicate mapping.

## Required ownership contract

Coexistence is valid only under all of these conditions:

1. `HALIOMMUDxe` runs before `SmmuCrDxe`, so Crane observes all mappings made
   during HAL startup.
2. Crane owns PCIe SIDs `0x1c00`, `0x1c01`, `0x1c80`, and `0x1c81` from attach
   through detach. No caller may pass those SIDs, or an overlapping mask, to
   the Qualcomm private HAL protocol during that interval.
3. HAL clients may continue to own other valid SMRs and context banks. Crane
   must continue to allocate only invalid SMRs and banks which pass its full
   in-use check.
4. Neither driver nor a later firmware component may globally reset the SMMU
   or rewrite unmatched-stream policy while either owner has live mappings.
5. A HAL detach/delete may operate only on resources allocated by that HAL
   domain. This is a software contract because the audited detach helper does
   not enforce ownership in hardware.

The private HAL protocol is not an implementation of
`EDKII_IOMMU_PROTOCOL`, so locating `gEdkiiIoMmuProtocolGuid` cannot detect its
presence or prove that it owns PCIe streams. The current target setting
`ExternalIoMmuOwnsPcieStreams = FALSE` reflects the binary evidence: HAL is
present, but no audited startup path claims PCIe. The generic MU PCI stack does
not call the private HAL protocol. Several packaged Qualcomm storage, USB, and
display drivers contain its GUID, however, so private-protocol use for their
own IORT components remains possible after Crane starts.

## Evidence limits and runtime checks

An older Qualcomm source tree was used to name the stripped routines and
cross-check their data structures. It is not treated as the source used to
build either Waipio binary. Addresses, comparisons, and register writes stated
above were checked in both packaged binaries; high-level names derived only
from the older source are not binary provenance.

Board validation still needs register snapshots of CR0, every valid SMR/S2CR,
and every used CBAR/CBA2R/context bank before HAL, before Crane, after PCIe
attach, and after PCIe detach. It should also trace calls to the private attach
and detach callbacks and verify that none resolves to the four PCIe SIDs while
Crane owns them. A successful firmware build or link enumeration alone does
not validate this ownership contract or DMA isolation.
