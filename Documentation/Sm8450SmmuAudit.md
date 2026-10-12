# SM8450 SMEM and IOMMU source audit — 2026-10-03

The current target is `qcom-hdk8450`. This audit covers the SMEM wire reader,
UEFI SMEM protocol, native SMMUv2 core/EDKII IOMMU adapter, and the separate
PrePi detach helper. Host fixtures and a firmware build do not establish
successful hardware translation or peripheral DMA.

## Supplied log

SMEM initializes successfully in both early and DXE captures: version 12,
644 items, 19 partition-table entries, global offset `0x1000`, size `0xAA000`.
The `[ERROR]` progress lines come from the enabled diagnostic level. The first
allocation failure was an unsupported allocator; the next was a duplicate
item 404 with no returned pointer. Neither establishes a corrupt SMEM region.
The current protocol reuses an existing adequately sized object under the
remote lock; the native HDK image also excludes the stock DALSys driver.

The latest capture reaches a successful SMMU probe with 113 streams, 75 banks,
4 KiB pages and ID2 `0x5111`. Probe/install success does not exercise attachment,
page-table walking, permissions, TLB completion, or real translated DMA.
Missing Reset/Monotonic architectural protocols are separate dispatch failures.
The restored HDK source manifests include both source producers.

## Existing work restored

The build-preparation stashes contained native resource owners, UFS/MDSS source
and their tests. They were restored by file, retaining the later SMEM/SMMU
safety patches and the HEAD GPT/PCI CPU I/O changes. No stash was popped or
dropped; no branch, submodule HEAD, commit or remote was changed.

- Parent source baseline: `8ff827075cc2ee394bdff00a018b80aae36f8e65`.
- Crane baseline: `6264406c0f754a1cf5f55e0aa974911e3ee3fc0a` (including its
  untracked third-parent tree).
- MU UFS: its `codex pre-build MU_BASECORE worktree 20261002` stash, whose
  three UFS files match the current HEAD before restoration.

HDK active DSC, FDF and APRIORI entries use source drivers. Panel/config raw
files remain data. The native UFS/MDSS/LT9611 entries and paired MMIO mappings
were restored together. MTP stock packaging is outside this HDK validation.

## Correctness findings and retained fixes

| Area | Defect and resulting behavior |
| --- | --- |
| SMEM bounds | Selected partitions and live uncached/cached cursors must not point into the 32-byte header. Malformed spans, overlaps, padding, cached strides and allocation overflow fail before a payload is exposed or metadata published. |
| SMEM concurrency | Publication words use aligned volatile accesses and barriers. DXE allocation serializes APPS calls and holds SM8450 TCSR lock 3, owner 1. A timeout does not write the SMEM heap; unlock does not clear a remote owner's lock. |
| SMEM UEFI allocation | Linux duplicate allocation remains `CR_BUSY`; `AllocFunc` translates an existing adequately sized item into success plus its original pointer. A smaller object fails with `EFI_BAD_BUFFER_SIZE`. |
| SMMU widths | ID2 UBS controls IOVA input, IAS controls stage-1 output/PASIZE, OAS controls table-walker physical bounds. The fixed L1/L2/L3 hierarchy advertises at most 39 input bits. Wider hardware input support does not silently select a missing L0 table. |
| SMMU geometry | Raw stream counts above the 128-entry reservation capacity are rejected without MMIO writes. Capping the count could hide a live route and permit reuse of its bank or ASID. The observed SM8450 count is 113. |
| SMMU ownership | Software reserves banks, streams and ASIDs, including failed attachments. A repeated probe cannot discard reservations or timeout state. Stage-2, enabled, referenced and Qualcomm final banks remain protected. |
| Programming failures | Register readback and masked TCR2 checks detect ignored writes. ACTLR is applied only by matching target rules. Failed attachment/detach retains ownership and tables; restoration orders interpretation controls before TTBR. |
| TLB failure | A timeout poisons the native controller; subsequent domains do not submit additional TLB commands or reuse resources. There is no shared-controller recovery/reset claim. |
| PCI/on-chip scope | PCI segment cleanup excludes UFS/GPI/MDSS domains. Reattaching a SID cannot change its PCI function or on-chip namespace. `FreeBuffer` refuses ranges still referenced by mappings, including failed unmap/bounce records. |
| Early firmware handoff | The PrePi helper must verify stream retirement and successful global TLB synchronization before disabling referenced eligible banks. It preserves kept MDSS routes, stage-2/foreign banks and the Qualcomm final bank. It leaves saved TTBR/TCR/CBAR configuration intact. SM8450 stops before DXE if this fence fails. |

The early helper uses architectural TLBIALLNSNH offset `0x68`, then global
TLBGSYNC `0x70`, and polls completion. A write to `0x70` alone is synchronization,
not invalidation. The keep list includes MDSS `0x2800/0x2802/0x2c00/0x2c02`.
Kept streams may share a context bank; that bank must remain live.

SM8450 MDSS ACTLR target rule is SID/mask `0x2800/0x0402`, value `1`.
The implementation verifies Linux-owned `PASIZE/AS/SEP` TCR2 fields and preserves
implementation bits. No cache-unlock or global reset workaround is introduced.
An occupied SID discovered later in DXE remains a collision; early handoff
does not authorize taking a resource from another live DXE driver.

## Validation

ASan/UBSan host fixtures exercise the real SMEM/SMMU implementations with
mocked memory/MMIO. Relevant runners include `run_smem_tests.py`,
`run_smem_sec_adapter_tests.py`, `run_smem_dxe_tests.py`, `run_smmu_tests.py`,
the early-helper regression, PCI cold/host tests, GENI/GPI tests, MDP GOP tests,
and UFS lifecycle/completion/resource-release tests. Successful tests establish
fixture behavior, not secure-world permissions or actual TBU activity.

The new SMEM protocol test checks the original item-404 duplicate trigger,
size mismatch, preserved payload, hardware/local contention and interrupt state.
The new stream-capacity test supplies a live route above index 127 on an
apparently cold controller and requires an error with zero register writes.

Fresh HDK8450 DEBUG/CLANGPDB build completed on 2026-10-03 with 126 images
validated. The generated artifacts are:

- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd` (4,194,304 bytes),
  SHA-256 `3e46f7b238e4ebc70dc7e646698f20463dfa87ebf51c4fe8cf72f9492c0584fa`.
- `Build/WaipioCrPkg/bootpayload.bin` (1,595,565 bytes), SHA-256
  `9a10fa46dd78cc5b782a1dd47b2db62896a77806819aed21b52acb710a46e285`.
- `Build/WaipioCrPkg/qcom-hdk8450.img` (1,601,536 bytes), SHA-256
  `b0a5cd38087c14fb014737d9c15c2b1f876397b6d67e1a9cb480528b4f1ea0e0`.

The build report lists native `SmemCrDxe`, `SmmuCrDxe`, `UfsHcCrDxe`,
`UfsPassThruDxe`, `MDSSCrDxe`, source Monotonic Counter and Reset drivers. The
active HDK manifest scan finds zero stock driver paths under `Binaries/` or
`PatchedBinaries/`; the only precompiled module is the shared MU BaseCrypto
driver, which is not a Qualcomm hardware owner. These artifacts are still
unproven on the board until the hardware procedure below is completed.

## Hardware test and remaining limits

Capture UART output from PrePi through BDS. Check early detach completion,
SMEM protocol install, native SMMU attach/readback/TLB results, native UFS
inquiry/read completion and MDSS/GPI results. Successful GOP publication alone
does not prove visible scanout. Record SID/CB/ASID and any timeout/readback
failure; repeated commands after a poisoned-controller message are a defect.

The current ExitBootServices callback revokes mappings while preserving a
reserved read-only framebuffer. It has no ordered handshake that stops every
UFS/GPI DMA engine before fencing. If the fence fails, page-table retention
alone does not reserve arbitrary caller or bounce pages for the OS. This is a
remaining shutdown/lifetime boundary, not a claim of full DMA containment.
Register access may also hang if clocks, power or secure firmware permissions
are wrong; host mocks cannot prove those preconditions.

## References

Local Linux revision `40288c9206c17eb66a603262e06a58d300d0f279`:
`drivers/soc/qcom/smem.c`, `drivers/hwspinlock/qcom_hwspinlock.c`,
`drivers/iommu/arm/arm-smmu/{arm-smmu.c,arm-smmu-qcom.c,arm-smmu-impl.c}`,
and `arch/arm64/boot/dts/qcom/{sm8450.dtsi,sm8450-hdk.dts}`.
The HDK DTS confirms LT9611 1.2 V GPIO9, 3.3 V GPIO109 and reset GPIO107;
PCIe0 DBI/config are `0x60000000/0x60100000`, PCIe1 `0x40000000/0x40100000`.

Android Qualcomm synchronization/handoff references retained from the prior
source audit: OnePlus SM8450 kernel `449760504b3a75728eb60479f5b34dc51cb263ce`
and Waipio devicetree `877203a7cf9b79973d44a9db716490f95d820110`. These identify
failure and ownership contracts; they are not proof of this board's behavior.
