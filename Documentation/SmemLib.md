# Crane SMEM reader

`Library/SmemLib/smem.c` ports the existing Aloha SMEM reader into Crane.
It was independently implemented using the wire format and lookup behavior in
Linux `drivers/soc/qcom/smem.c`, revision
`40288c9206c17eb66a603262e06a58d300d0f279`. It uses the existing OSKAL types,
status codes and platform memory barriers; it does not require UEFI Boot
Services, a memory-map lookup or a CrDAL protocol.

The public interface is `Include/Library/smem.h`:

- `SmemInitialize(Context, Base, Size, PhysicalBase)` validates a caller-owned
  mapped region and records its global/private partition layout.
- `SmemGetItem(Context, Host, Item, Size, Pointer)` finds an existing item.
  `Size` is optional. Output arguments are cleared on failure.
- `SMEM_HOST_ANY` selects global lookup. A present private partition is searched
  without falling back to the global partition after an item miss, as in Linux.
- `SmemAllocateItem(Context, Host, Item, Size, Pointer)` creates an item using
  the Linux v12 private-entry format, or the v11 legacy global heap when no
  partition table is available. It publishes metadata before advancing the
  free cursor and returns `CR_BUSY` when the item already exists.

The mapping must be coherent or uncached normal shared memory. Allocation
metadata is read with aligned volatile accesses and an acquire barrier after
the publication flag/free offsets. The caller must keep the mapping/context
alive and reinitialize after a firmware reset or SMEM reformat. Returned payloads
remain shared; payload synchronization belongs to their consumers.

## Formats and bounds

The reader supports v11's 512-entry global heap and optional private partition
table, and v12's global/private partitions with an optional SIII item-count
record. Uncached items grow upward, while cached items are traversed downward
with the partition's cache-line alignment. Only one mapped region is supported;
an auxiliary base whose low 32-bit selector does not match the mapped physical
base is rejected, matching Linux's 32-bit SMEM `aux_base` comparison.

Initialization and lookup check the region span, published initialized/reserved
fields, version, partition table extent, selected partition bounds and headers,
free cursors, canaries, padding and complete item extents. Only the global
partition and APPS-related host pairs are selected, matching Linux; stale
entries for unrelated host pairs are ignored. Relevant duplicate/overlapping
partitions are rejected, while optional SIII metadata falls back to the Linux
512-item default when absent or invalid. Malformed lengths cannot advance a
cursor outside its partition. The reader retains the previous library's
25-host bound and requires at least 8,400 bytes for the main region. The SM8450
region is 2 MiB, so this minimum does not limit the target integration.

The allocator follows Linux's publication ordering and leaves newly allocated
payload bytes for the caller to initialize. `SmemAllocateItem()` itself does
not lock: its caller must hold the SMEM remote hardware lock and serialize
local callers for the whole operation. `SmemCrDxe` performs that integration
with an atomic local re-entry guard and the SM8450 TCSR mutex (resource
`0x01f40000`, lock 3 at `0x01f43000`, Apps owner value `1`) before publishing
the legacy UEFI SMEM protocol. It does not load the Qualcomm `SmemDxe` binary.

## UEFI integration

`MUCommonCrPkg/Library/SmemSecLib` is now a thin memory-map adapter. Its existing
`SmemLibInit()` and `SmemGetItemPtr()` signatures remain available, and its
context aliases the Crane context. Existing source callers are rebuilt with
that header; the old raw context layout is not a binary protocol ABI.

The adapter resolves the named `SMEM` region, validates its address/length and
delegates all parsing to `SmemLib`. `SmemCrDxe` performs the same mapping in DXE
and produces `gEfiSMEMProtocolGuid` for existing consumers such as
`AcpiPlatformUpdateLib`; HDK8450 no longer packages the original Qualcomm
`SmemDxe`. SM8150's PlatformPei consumer continues to use this adapter.
SM8450 PrePi calls the early SMMU detach helper; it does not initialize
SMEM. DXE initializes the SMEM reader when publishing its protocol. The adapter
remains available to source consumers on other platforms. HDK8450's existing `0x80900000`/`0x200000` uncached region
matches Linux `arch/arm64/boot/dts/qcom/sm8450.dtsi`; no second base is invented.

## Validation

`Tests/run_smem_tests.py` exercises the actual core against synthetic v11/v12
images, private/global lookup and allocation (including item 404), cached
reverse traversal, missing items, optional sizes, malformed metadata, overflow
and unsupported auxiliary regions.
`Tests/run_smem_sec_adapter_tests.py` links the real adapter and core to check
region lookup errors, invalid spans and compatibility calls. Both use
ASan/UBSan and treat compiler warnings as errors.

`Tests/run_smem_dxe_tests.py` links the production DXE protocol and core.
It checks existing item 404 reuse, requested-size validation, hardware-lock
contention without heap writes, local reentry rejection, remote-owner
preservation on unlock, and restoration of the interrupt state. These use
mocked MMIO; they do not prove that the board permits TCSR mutex writes.

The core retains Linux's duplicate-allocation `CR_BUSY` result (mapped to
`EFI_ACCESS_DENIED` in OSKAL). The UEFI `AllocFunc` implements get-or-allocate:
a duplicate returns the existing pointer only when its published size is at
least the requested size; a smaller object returns `EFI_BAD_BUFFER_SIZE`.
No duplicate is appended, and existing payload bytes are preserved.

The supplied SM8450 logs already show successful v12 initialization, 644 items,
and the global partition at offset `0x1000`, size `0xAA000`. Their diagnostic
`[ERROR]` prefix is the configured log level, not a failed parser status. The
older DAL item-404 failure is an allocation-contract issue. HDK no longer
packages the stock DALSys consumer.

These fixtures validate parser and protocol behavior. Firmware construction
and hardware validation are recorded in [the platform audit](Sm8450SmmuAudit.md).
