# UsbMscCrLib

Portable SCSI/Bulk-Only Transport core for the `UsbMscTest` UEFI
application. The original `CrUsbMscInit` formats a caller-owned 16 MiB RAM disk. Sector zero contains an
MBR; a FAT16 partition begins at LBA 2048 and contains `README.TXT`. The volume
has two FATs, 512-byte sectors and 1024-byte clusters. This mode writes RAM only.
`CrUsbMscInitBlock` instead exposes one caller-selected block medium through
synchronous callbacks. `CrUsbMscInitBlocks` exposes one or two media as USB
LUNs 0 and 1, in descriptor order. Neither block initializer formats media.
The application chooses physical devices; the core does not discover storage.

The core uses OSKAL types and status values, with no operating-system calls,
allocation, MMIO, hardwired storage-provider or USB controller dependencies. Native
and NT host builds exercise the same C source. The application owns USB
control requests, descriptors, asynchronous transfer buffers, cancellation,
cable events, configuration and final controller shutdown.

## Adapter contract

Use zeroed, stable `CR_USB_MSC` storage and a disjoint disk allocation for RAM mode.
`CrUsbMscInit` formats the disk once. `CrUsbMscSetMaxPacket` selects the current
bulk maximum packet size after bus configuration. Every call is serialized.

`CrUsbMscInitBlocks(Context, Backends, Count)` accepts one or two disjoint
descriptors, validates every descriptor before changing the core, then copies
them. Initialization performs no backend callback. `CrUsbMscGetMaxLun` returns
the last configured USB LUN for the control request. LUN numbering and geometry
remain fixed until a newly initialized core is selected. The core has one
sequential BOT transport and one staging buffer shared by all LUNs.

Sense, media-error latches, logical eject and prevention of removal are stored
per LUN. Errors or removal on one LUN do not redirect access to another.
An unsupported CBW low-nibble LUN fails ordinary commands with sense 5/25/00;
REQUEST SENSE retrieves that diagnostic, and valid standard INQUIRY returns
PQ=3/PDT=1f. Neither path invokes storage callbacks. Reserved high LUN bits
require transport recovery. REPORT LUNS reports the configured list in
peripheral addressing format; select-report 1 returns no well-known LUNs.
VPD page 80 serials end in `1` and `2` for LUNs 0 and 1. These identify the
configured USB slots, not the physical storage device's hardware serial.

`CrUsbMscNext` returns one bulk IN, bulk OUT, endpoint-stall or no-action result.
The adapter copies through its controller-owned buffers and calls `Complete`
only after that action completes. An IN action of length zero is an explicit
ZLP. Disable automatic bulk-IN zero-length termination. A buffer remains owned
by its action until completion; do not call `Next` with an outstanding action.
OUT results must be copied to the supplied core buffer before completion.
Transfer chunks never exceed 64 KiB.

For block mode, pass an immutable `CR_USB_MSC_BLOCK_BACKEND` descriptor with
`Context`, `BlockSize`, `LastBlock`, `ReadOnly`, `Read`, `Write`, `Flush`, and
optional `Check`. The descriptor is copied. Its context must remain valid
until the core and outstanding transfers are retired. The block size must be
a power of two from 512 through 65536 bytes. `Read` is required; writable
media also requires `Write` and `Flush`. Read-only media may omit the latter
two callbacks. Each read/write receives a 64-bit LBA, a positive block count,
and at most 64 KiB of internal staging memory. Callbacks must complete
synchronously and must not retain the staging pointer. They execute from
the serialized `Next`/`Complete` caller, never a separate USB callback thread.

Only exact `CR_SUCCESS` indicates a complete callback. Map no media to
`CR_NOT_FOUND`, changed medium to `CR_ABORT`, write protection to `CR_BUSY`,
temporary not-ready to `CR_NOT_READY`, and other I/O failures to
`CR_DEVICE_ERROR`. These become SCSI sense and command-failed CSWs while
preserving BOT phase-error status when required. No-media/changed errors
are latched across USB/BOT resets. The adapter must pin the selected media
identity and reject a changed MediaId rather than adopting another card.
Use a newly initialized core to select replacement media. A latched removal
or change remains local to that LUN and survives both kinds of reset.

`Check`, when supplied, tests that pinned medium for admitted TEST UNIT READY
and READ CAPACITY10/16 commands. Malformed requests and wrong-direction BOT
requests do not call it. Valid no-data commands still execute when the host
declares excess IN or OUT data (BOT cases 4/9): TEST UNIT READY calls `Check`
and SYNCHRONIZE CACHE calls `Flush`. Failures produce failed CSWs, with the
full host-byte residue retained through the IN halt or OUT discard.
Without `Check`, removal is detected lazily by the
next read/write/flush callback. Cached geometry remains immutable. READ
CAPACITY10 saturates at `0xffffffff`; READ CAPACITY16 retains the entire
64-bit last LBA. MODE SENSE reports write protection and disables write cache
(WCE=0). Successful write chunks are flushed before being counted as processed
and before success is reported. SYNCHRONIZE CACHE10/16 also calls `Flush`.
A zero block count for these commands means through the end of the medium;
the starting LBA must still be in range. Zero-block READ/WRITE remains a no-op.

Write commands with a short CBW host length or wrong direction perform no
backend I/O. A received write chunk must complete in full before it is passed
to `Write`. Streaming writes are not atomic: earlier flushed chunks can
remain on disk if a later transfer or callback fails, and a failing provider
may have partially modified a chunk. CSW residue conservatively retains any
chunk that did not complete both write and flush. Remaining OUT bytes are
discarded without further storage calls. The core never silently retries a
failed write against different media.

The adapter must abort and drain outstanding controller requests before class
reset, bus reset or disconnect. A failed/aborted `Complete` returns a failure
status and seals the command into reset recovery. Class reset preserves
endpoint halts and waits for both successful clear-halt requests. Bus reset
clears transport state and preserves disk contents. Both resets clear every
LUN's prevent-removal state. Class reset preserves each LUN's sense and eject
state; bus reset clears them. Both retain immutable backend descriptors and
latched media errors. `CrUsbMscClearHalt` returns
false when the core requires a halt to remain; do not clear the hardware halt
in that case. A failed hardware halt/clear operation requires transport
shutdown; software state cannot prove the controller changed successfully.

The 13 BOT host/device direction-length cases are handled explicitly. Short
IN replies include a ZLP when needed, then an IN halt until the host clears it;
CSW residue counts relevant data. Excess or incompatible OUT bytes are accepted
into a bounded discard buffer, avoiding a race with already-acknowledged USB
packets. Short-host WRITE commands never modify the RAM disk. Phase-error CSWs
require reset recovery, while malformed CBWs produce no CSW and halt both
bulk endpoints until reset recovery.

Supported commands include INQUIRY and VPD 00/80, REQUEST SENSE, TEST UNIT READY,
READ CAPACITY 10/16, READ FORMAT CAPACITIES, READ/WRITE 10/12/16, MODE SENSE 6/10,
START STOP UNIT, PREVENT/ALLOW MEDIUM REMOVAL, VERIFY 10 without BYTCHK in RAM mode,
SYNCHRONIZE CACHE 10/16 and REPORT LUNS. Unsupported commands and invalid fields
produce SCSI sense and command-failed status. Block mode rejects VERIFY 10
with unsupported-command sense; physical verification is not implemented.
RAM mode has no persistence;
block mode persistence depends on the selected provider honoring successful
write/flush completion.

## References and validation

The implementation is original MIT-licensed code. Protocol references:

- [USB-IF Bulk-Only Transport 1.0](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
  sections 5 and 6: wrappers, reset recovery and direction/length matrix.
- [Android SM8450 gadget mass-storage reference](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/usb/gadget/function/f_mass_storage.c),
  `finish_reply`: excess-OUT discard avoids the endpoint-halt/ACK race. Its
  optional no-stall short-IN mode explicitly departs from BOT; this core uses
  the specified short/ZLP and halt sequence.
- [T10 SBC-3 working material](https://t10.org/ftp/t10/document.05/05-344r0.pdf)
  and [SCSI sense-key definitions](https://t10.t10.org/lists/2sensekey.htm).

Run `python3 Platforms/CranePkg/Tests/run_usb_msc_tests.py --output <directory>`.
It builds the actual core twice with ASan/UBSan, tests transport recovery and
all 13 cases, enumerates 4096 CDB opcode/length combinations, checks streamed
read/write and sense behavior, and independently validates the generated FAT16
volume with `fsck.fat`, `mdir` and `mtype`. These checks do not establish physical
USB enumeration; the UEFI adapter and board still require device testing.

Run `python3 Platforms/CranePkg/Tests/run_usb_msc_block_tests.py --output <directory>`
for native/NT ASan/UBSan tests against sparse media above 2 TiB, 4 KiB and
64 KiB sectors, UINT64 boundary LBAs, overflow rejection, all 13 BOT cases,
read-only protection, media checks/latching, read/write/flush failures,
physical VERIFY rejection, no-data BOT command execution and SYNC range checks,
streamed data integrity, durable-write ordering and phase-error recovery.
The receipt freezes the actual compiler dependency closure and output hashes.

Run `python3 Platforms/CranePkg/Tests/run_usb_msc_multi_tests.py --output <directory>`
for native/NT ASan/UBSan tests of one/two-LUN admission, GetMaxLun, list/serial
enumeration, all unsupported low-nibble LUNs, reserved-high-nibble recovery,
mixed 512/4096-byte media with 64-bit LBAs, streaming writes and flushes,
write protection, independent sense/eject/prevention, latched removal/change,
read/write/flush faults, and resetting while a drained OUT action is pending.
The original RAM tests retain the same case count; the malformed-wrapper
test now sets a reserved high LUN bit instead of using an unsupported LUN.
