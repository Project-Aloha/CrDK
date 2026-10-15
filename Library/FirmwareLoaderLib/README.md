# Shared Android firmware reader

`FirmwareLoaderLib` is the single portable parser and selection core used by
Windows `NhlosPartition` and the UEFI FirmwareLoader adapter. It originated in
the existing `CrWDDK/NhlosPartition/Core` implementation. `PROVENANCE.json`
records the migration and original source hashes; no proprietary BSP code is
used. The Windows adapter must compile the pinned CrDK copy of these sources,
not maintain another parser.

The library has no disk write function, native handles, device numbers, global
search path, mutable global state, or hardware programming. Callers supply
exact bounded reads and an allocator. The same source compiles as C17 for host
tests/Windows and uses BaseLib/BaseMemoryLib for UEFI. `NH_HOST_TEST` enables
host standard integer types when a test also includes UEFI mock headers.

## Interfaces and ownership

- `NhGptFind` reads both GPT headers and arrays, validates CRCs, matching layout
  identity, usable ranges, partition overlap/duplicates, and reports a byte
  slice. A valid GPT missing the name returns `NH_NOT_FOUND`. Two absent GPT
  signatures without a protective MBR return `NH_UNSUPPORTED` for raw boot
  LUNs. A damaged/unknown GPT layout is `NH_CORRUPT`; I/O is preserved.
- `NhSuperOpen` opens one named logical partition at an explicitly selected
  metadata slot. It validates the liblp format and creates a bounded extent
  view. `View.Flags` survives `NH_NOT_FOUND` to support update admission even
  when the requested logical partition is absent. It does not resolve COWs.
- `NhFsOpen` and `NhFileOpen`/`NhFileRead` support read-only FAT12/16/32, ext4,
  F2FS and EROFS subsets. Reads are bounded by the declared filesystem as well
  as the containing partition. Unsupported modes and symlinks are errors.
- `NhBundleRead` returns one private file or PIL MDT/MBN plus required bXX
  members. All members come from one source callback. A missing primary is
  absence; missing secondary members are corruption. `NhBundleFree` releases
  the group with the same allocator context, which must outlive that group.
- `NhPolicyRead` validates the entire ordered Android/package policy before
  I/O. Its callbacks bind the actual device, open one candidate, recheck stable
  device/slot/update state, and open only the consumer's own package. Only
  `NH_NOT_FOUND` advances the list. Empty Android paths explicitly select
  package-only; provider failure does not imply this policy. Returned Origin
  identifies Android or package, and the optional index identifies the winning
  list entry. The adapter retains hardware handles only until `End`.

UEFI may stream a large PIL group directly into its reserved carveout instead
of allocating every payload. It must retain one filesystem/root source token
for the complete group, validate required headers before selection, prohibit
fallback after selection, and report a missing member as corruption. Related
main/DTB images must use the same selected source. PIL authentication and load
addresses remain consumer responsibilities.

Candidate syntax is `physical|logical-or-empty|relative/path`. A logical
partition requires physical `super`. `{slot}` may occur only at the end of a
partition name and expands to `_a` or `_b`; an explicitly opposite slot is an
error. Both the current device and slot are selected by the adapter, never by
an arbitrary consumer-supplied raw offset. Policy paths and device identity
belong in target/INF configuration, with `super|vendor{slot}|...` first where
qualified. No shared parser branch names a board model.

## Android update admission

The current boot slot comes from the immutable bootloader handoff. GPT active
attributes can select the next boot and must not redirect this boot's firmware
reads. `NhBootControlSlot` validates the specific AOSP CRC-protected structure;
its existence does not make `misc + 2048` a universal Qualcomm slot authority.
`NhBootArgsSlot` parses supplied command-line slot keys. The native adapter's
AndroidBootCr parser additionally handles verified bootconfig and FDT values,
rejecting contradictory sources instead of choosing an arbitrary one.

`NhBootSlot.h` defines a 32-byte little-endian current-boot record for the
`CrAndroidBootSlot` UEFI variable. The producer publishes it with boot-service
and runtime access, without nonvolatile storage. Windows captures it once at
driver initialization. `NhBootSlotDecode` rejects wrong length, signature,
revision, attributes, state, slot, source bits, initialization flags and
reserved fields. Missing or explicitly absent slot authority is not a missing
firmware file and cannot authorize package fallback. This transport is
provenance metadata, not an authentication mechanism. Stable storage identity
and the independent snapshot check below remain adapter responsibilities.

Runtime adapters currently require a valid 64-byte `misc_virtual_ab_message`
with version 2, magic `0x56740AB0`, merge state `NONE`, and zero reserved bytes
for **every** super access, including logical-partition absence. AOSP explicitly
notes that retrofit Virtual A/B devices can lack the expanded liblp header, so
an unset Virtual A/B flag cannot establish that base extents are current.
Uninitialized/all-zero misc, unknown state/version, snapshots, merging,
cancelled state, and concurrent OTA writers are not admitted. These errors do
not authorize modem/package fallback. Version-1 messages and legacy devices
without this message are not currently qualified for runtime super access;
support requires an explicit independently justified non-VAB admission policy.
The offline LP parser can still inspect base images for format testing; that
does not establish runtime snapshot safety.

## Format limits

ext4 supports direct/indirect and extent mappings plus metadata checksums on a
clean filesystem; no journal replay, fscrypt, inline-data or bigalloc. F2FS
requires a clean checkpoint and supports NAT/compact journal, direct/indirect
and inline data/dentries; encrypted/compressed/casefolded files are refused.
EROFS supports 4 KiB flat/inline and legacy LZ4/compact indexes; no big-pcluster,
fragments, tailpacking, extra devices or newer compression algorithms. FAT
validates ASCII short/long names, mirrors, clean state and complete bounded
cluster chains; a factory image may omit unused tail bytes. liblp supports v10
through minor 2 with one physical super device and LINEAR/ZERO extents.
Android sparse RAW/FILL/DONT_CARE exists for offline fixtures; CRC sparse
streams are unsupported and runtime block devices must already be unsparsed.

## Tests and provenance

`python3 Tests/FirmwareLoader/run_tests.py --output <new-directory>` runs the
actual core under ASan/UBSan. Optional `--images`, `--reference` and
`--f2fs-reference` compare supplied images to an independently extracted
length/SHA256 manifest without copying firmware payloads.
`Tests/FirmwareLoader/filesystem_fixtures.py` tests independently generated
filesystem images. Adapter lifetime/identity checks and ARM64 WDK or UEFI image
builds are separate from parser tests; hardware success requires board logs.

Most sources are MIT. **NhErofs.c remains GPL-2.0-or-later**, retaining its
U-Boot compact-index decoder attribution. Linked distributions must respect
that license; this migration does not relabel it BSD/MIT. License texts are in
`LICENSES/`. Tests also preserve their existing file-level SPDX identifiers.

Public format references:

- [AOSP liblp Android 15](https://android.googlesource.com/platform/system/core/+/android-15.0.0_r1/fs_mgr/liblp/include/liblp/metadata_format.h)
- [AOSP bootloader message Android 15](https://android.googlesource.com/platform/bootable/recovery/+/android-15.0.0_r1/bootloader_message/include/bootloader_message/bootloader_message.h)
- [UEFI GPT format](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html)
- [Linux ext4 format](https://docs.kernel.org/filesystems/ext4/index.html)
- [Linux v6.6 F2FS format](https://github.com/torvalds/linux/blob/v6.6/include/linux/f2fs_fs.h)
- [Linux EROFS format](https://docs.kernel.org/filesystems/erofs.html)
- [U-Boot EROFS decoder revision](https://source.denx.de/u-boot/u-boot/-/blob/e37de002fac3895e8d0b60ae2015e17bb33e2b5b/fs/erofs/zmap.c)
