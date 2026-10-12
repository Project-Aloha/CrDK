# RAM partition discovery

`RamPartitionCrLib` decodes SMEM item 402 into bounded, sorted APPS system
SDRAM banks. It performs no allocation, mapping, GCD publication or hardware
write. The DXE caller owns the SMEM lease while copying the remote item into
stable storage and then calls `CrRamPartitionParse` on that copy. The returned
table is committed only after every selected range has been validated.

The existing [EFI RAM protocol](../../QcomModulePkg/Include/Protocol/EFIRamPartition.h)
exposes `AvailableLength`; the existing Andromeda `RamPartitionDxe` consumes
that protocol, rather than parsing SMEM itself. Those files are unchanged.

The wire fields are little endian. They are read bytewise to support unaligned
copied data. Partition names occupy 16 bytes and need no null terminator.

| Version | Header bytes | Entry bytes | Base/length | Available length |
| --- | ---: | ---: | --- | --- |
| 0 | 20 | 56 | 32 bit | Physical length |
| 1 | 24 | 64 | 64 bit | Physical length |
| 2 | 24 | 72 | 64 bit | 64 bit at entry offset 64 |
| 3 | 24 | 72 | 64 bit | 64 bit at entry offset 64 |

The magic words are `0x9da5e0a8` and `0xaf9ec4e2`. The count is at offset 16
and may not exceed 32. Trailing allocation padding is allowed; absent declared
entries and unsupported versions fail rather than selecting a guessed stride.
Version 3 defines HighestBankBit at offset 56 and MinPasrSize (MB) at offset 60.
Version 2 labels those eight bytes reserved, so its normalized values are zero.

Selection requires Category 14 (SDRAM) and Type 1 (system memory). Domains 0
(unspecified), 1 (APPS) and 3 (shared) are selected; explicit modem-only domain 2
is excluded. The Qualcomm-authored U-Boot decoder and pinned LK DDR predicate
select system SDRAM without requiring an APPS domain bit. Domain 0 does not
prove a modem-only allocation; availability and firmware reservations still
bound every usable range. Capture39 returned no selected entries with the
earlier APPS-only filter, but its actual wire fields have not yet been captured.
Unknown domain bits on system SDRAM fail validation. Attributes remain raw:
0 means read-only and 1 means read/write; consumers must enforce their own
writable-memory policy. No default attribute is converted into read/write.

The physical extent and available extent must be page aligned and nonwrapping.
AvailableLength may be zero and cannot exceed Length. A zero-length record
with zero availability is skipped after domain/alignment validation; it is
not a bank and owns no bytes. HDK8350 v3 logs include such a record inside a
real bank's unavailable tail. An all-empty table returns NOT_FOUND. Full physical extents
must be disjoint even when their usable prefixes do not overlap. Adjacent
banks retain independent metadata and unavailable tails. PhysicalBytes sums
physical Length; AvailableBytes sums AvailableLength. Neither is an OS grant.

Version 2 and later distinguish full physical DDR from a usable prefix after
dynamic carveouts. A zero or shorter available length must never be replaced
by physical length. The native consumer must also subtract every board/firmware
reservation, including reservations absent from HOBs, before adding RAM to GCD.
An output bank's unavailable `[Base+AvailableLength, Base+Length)` tail must
remain reserved. This parser makes no fixed RAM density assumption.

Format evidence:

- [Qualcomm-authored U-Boot RAM table definitions and parser proposal](https://www.mail-archive.com/u-boot%40lists.denx.de/msg577989.html)
  defines v0/v1/v3, the magic/count/domain/type values, and 16-byte raw names.
  This is a published patch, rather than a claimed merged commit. The local
  SM8450 Linux and Android kernels do not contain a RAM item 402 decoder.
- [LK definitions at pinned commit 5e6f1adcb1c070e28a70dc1f3d807ce9538f61c5](https://github.com/msm8916-mainline/lk2nd/blob/5e6f1adcb1c070e28a70dc1f3d807ce9538f61c5/platform/msm_shared/smem.h#L620-L694)
  independently defines v0/v1/v2 strides and v2 AvailableLength. Local exact
  source is `.tmp/waipio-continuation/ram-partition39-ref/lk-smem.h`.
- [Qualcomm contributor's 2026-07-13 available-length correction](https://www.mail-archive.com/u-boot%40lists.denx.de/msg580501.html)
  explicitly explains that physical length includes dynamic/hypervisor
  carveouts and uses AvailableLength for v2+. The proposal above had used
  physical length; this parser preserves both and selects available length.
- [Linux SMEM consumer interface](https://github.com/torvalds/linux/blob/master/include/linux/soc/qcom/smem.h)
  uses host-scoped lookup with a returned item length. Crane's existing
  `SmemGetItem` / `EFISmemCrProtocol.GetItem` supplies the corresponding
  bounded item; the SMEM heap partition table itself is a different format.

Run `python3 Platforms/CranePkg/Tests/run_ram_partition_tests.py --output DIR`.
The actual core is compiled under native and NT APIs with ASan/UBSan. Independent
wire fixtures cover every supported stride, >4 GiB banks, all 32 banks, ignored
domains/categories, every truncation, every wire-bit mutation, unavailable-tail
overlaps, zero availability, failed output preservation and pointer alias/wrap.

## Native HDK8450 integration

`RamPartitionCrDxe` runs after CpuDxe and SmemCrDxe, before peripheral resource
allocation. It reads the scoped global SMEM item into a bounded private copy,
validates the complete table, and preserves every platform descriptor regardless
of HOB type. It also skips existing GCD memory, reserved regions and MMIO. Only
read/write APPS banks with nonzero available length can supply additional RAM.

New ranges are mapped WB/XP before `AddMemorySpace(SystemMemory)` makes them
allocatable. TPL_NOTIFY defers memory-map-change callbacks until the GCD attribute
update completes. Each at-most-two-GiB addition obtains a fresh GCD descriptor;
failed later additions do not remove memory that has already become allocatable.
The current physical and virtual address widths bound the entire available map
before any publication. Missing or unsupported SMEM data adds no guessed DDR.

The driver then publishes the existing EFI RAM partition protocol for SMBIOS
and other consumers. Its inventory reports usable bank lengths before the
platform exclusions, while the `added` log reports only new allocator memory.
Version/bank/PASR metadata is exposed only where the wire format supplies it;
inconsistent per-bank metadata returns unsupported. The old Andromeda RAM
expansion driver remains disabled on HDK8450 to avoid duplicate expansion.

`run_ram_partition_dxe_tests.py --output DIR` builds the actual driver and parser
in debug/release modes under ASan/UBSan. It covers sparse banks above four GiB,
unavailable tails, overlapping platform exclusions, reserved and MMIO GCD spans,
mapping/publication ordering, protocol sizing, and every callback failure.
Board confirmation still requires the boot log and shell `memmap` output.
