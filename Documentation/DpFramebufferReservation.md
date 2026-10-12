# HDK8450 fixed DP framebuffer backing

The selected HDK8450 memory map reserves `[0xF1400000, 0xF1C00000)` before
MemoryPeim. `HDK8450 DP Framebuffer` is an 8 MiB `AddMem/MEM_RES/Reserv` row
with the existing tested normal-RAM capabilities. The conventional remainder
starts at F1C00000, has length 0E000000 and still ends at FFC00000. DSI's
B8000000+02B00000 row, SMMU table ownership and the UEFI FD are unchanged.

This first milestone changes backing ownership. DP still stops at EBS and its
ordinary SMMU mapping is revoked. It does not yet request preservation or grant
Windows a joint DP/controller/PHY adoption path.

## Selection and current-boot qualification

`DpReservation.c` resolves only the unique `HDK8450 DP Framebuffer` row from the
actually selected `PlatformMemoryMapLib`. It verifies its complete profile,
64 KiB alignment, exact capacity, overflow and absence of overlapping map rows.
Other targets have no address fallback. The library selection in the platform
build determines the profile; neither the helper nor GOP guesses a SoC.

The new Crane `EFI_CR_RAM_RANGE_PROTOCOL` is published alongside the unchanged
Qualcomm RAM protocol after RamPartitionCrDxe has parsed its private SMEM402
snapshot, checked address width and finished its existing GCD additions. Its
query requires one normalized APPS/shared/default-domain RW bank to cover the
whole requested range using **AvailableLength**, including a zero grant. It
allocates/maps/writes nothing and grants no cache or DMA access. The immutable
query is unavailable before full inventory qualification. Reentry cannot replace
the snapshot after publication can be observed. A failed publication with a
nonzero returned handle retains the driver image and records the actual status.

DP caches that exact-revision query and qualifies the entire 8 MiB before the
GCD claim and again before cache cleaning or pixel access. Missing/malformed
protocols, short or read-only grants and nonfinal statuses cannot authorize
pixels. The static slice stays Reserved even when DP is unavailable.

## Reserved extent, cache and lifetime

The helper claims the fixed extent using `AllocateMemorySpace` with
`EfiGcdAllocateAddress`, `EfiGcdMemoryTypeReserved`, alignment16 and the actual
DP image handle. Fresh GCD observations must cover the whole extent, retain the
Reserved type, have the exact image owner and no device owner, support WC/XP,
carry PRESENT/INITIALIZED/TESTED and prohibit no CPU read/write access. Only
exact EFI_SUCCESS admits the next phase; warnings become NOT_READY.

The unchanged Core converts the static MEM_RES HOB to GCD Reserved. Its HOB
capability converter retains the three state bits for Reserved resources;
`PromoteMemoryResource` selects initialized **untested** resources, so this
tested extent is not boot-pool RAM. `CoreGetMemoryMap` projects the Reserved GCD
span as `EfiReservedMemoryType`. There is no Core patch, protocol, map-key oracle,
new accepted-map claim or runtime table hook.

GOP changes attributes and checks readback across the entire 8 MiB to WC/XP.
AArch64 WC is the existing normal noncacheable CPU mapping. Actual
1920x1080 BGRX pixels and READ-only DMA occupy 0x7E9000 bytes (2025 pages);
padding remains within the 2048-page reservation. This keeps cache memory type
uniform across every 64 KiB reserved granule. The fixed backing never uses
AllocatePages or FreePages.

Claim, attribute and release effects are recorded before invoking a mutator.
Errors or warnings may have committed: retries use fresh exact observations and
retain unknown effects, including a changed allocation output address. A foreign
owner is never adopted or freed. Before GOP publication, the driver first stops
hardware/retires its ordinary DMA, restores attributes with exact readback and
releases only its GCD claim. Once release is attempted, retries perform no further
attribute IO. FreeMemorySpace leaves the underlying Reserved resource intact;
no RemoveMemorySpace or type conversion occurs. Published or uncertainly
published GOP storage remains resident across unplug/retrain and EBS. EBS does
not allocate, free, restore attributes or withdraw the reservation.

## Validation and limits

`Tests/run_dp_tests.py --output <new-directory>` exercises actual DP/GOP paths,
actual FrameBufferBlt and the new reservation helper under ASan/UBSan. The focused
fixture uses the actual RAM query method and covers profile/grant/type/extent/
owner/capability rejection, warnings, mutation effects before and after failure,
exact cleanup retry, unchanged padding, retained publication and EBS. The actual
HDK map is separately compiled with the platform AddHob helper and bounded
unchanged Core capability/projection/promotion source slices. That test proves
these transformations; it is not a full Core/firmware execution test.

`Tests/run_ram_partition_dxe_tests.py --output <new-directory>` covers the actual
SMEM parser/DXE, the query's AvailableLength/RW rules and publication retention,
plus existing sparse low/high RAM and actual SMMU WC compatibility. Both runners
record each translation unit's actual preprocessing dependencies. Five separate
production ARM64 COFF/LTO objects were compiled using cached Native71 build flags
and AutoGen declarations; this is an object/ABI check, not a fresh module link.
Root coordinates the full firmware build and exact PE/FV/FD audit.

Current captured SMEM bank1 and the effective Android HDK DT both cover this
range as normal RAM; current-boot query remains mandatory. A separate existing
mismatch remains: Android's `oem_vm_region@bb000000` is BB000000+05000000 `no-map`,
while the UEFI `OEM VM` row is Conventional. This milestone neither uses that
range nor changes its policy. Live Windows DP still requires a qualified joint
controller/PHY/DMA adopter; fixed Reserved backing alone does not permit it.
