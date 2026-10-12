# UsbMscTest

Manually launched USB device-mode application for HDK8450. No arguments are
needed: it automatically exports **UFS hardware LUN0** as USB LUN0, and the
**SD card, if present at startup**, as USB LUN1. Both disks are read-only.
The existing partition tables and files are exposed without formatting.
Other UFS LUNs, NVMe and partition aliases are excluded. There is no RAM-disk
fallback or device-selection command line.

## Test on the board

1. Insert the SD card before launching the application if it should be exported.
2. In UEFI Shell, run `map -r` and switch to the filesystem whose device path
   is `Fv(...)` and which contains `UsbMscTest.efi` (often `FS0:`).
3. Run `UsbMscTest.efi` with no arguments.
4. Connect the HDK Type-C port directly to a computer using a USB data cable.
   The app prints each disk's mapping and waits for the platform's device role.
   The computer should enumerate one disk, or two when the SD card is present.
   Inspect the disks and read/copy existing files to the computer.
5. Eject/unmount the disks on the computer, then press `Esc` or `Q` on the UART
   console to stop the app and return to Shell. Run `map -r` again.

Only filesystems supported by the computer appear in Explorer. An Android UFS
LUN can enumerate correctly without a Windows-readable volume. The same USB
controller cannot also accept input from a USB host keyboard in device mode.
The app is packaged in FV and is also available as
`Build/WaipioCrPkg/UsbMscTest.efi`; it runs only when launched.

## Storage ownership and media changes

The app identifies the actual UFS device-path LUN number; enumeration order and
Shell `FSx:` / `BLKx:` indices do not select the disk. UFS LUN0 is required. If it
is absent, candidates are ambiguous, or a present disk cannot be acquired, the
app returns an error before starting USB and releases any acquired disk.

Exclusive BlockIo ownership detaches local filesystem consumers before USB
starts and reconnects them on exit. Launch from FV and close open files on the
exported disks. Each disk's media ID and native geometry are pinned for this
invocation. Removal or replacement invalidates only that disk until the app is
restarted, including across USB reconnects. An SD card inserted after app
startup requires restarting the app to add its LUN.

Reads use aligned 64 KiB buffers, native sector sizes (including 4 KiB UFS
sectors), 64-bit LBAs and READ CAPACITY(16). Every I/O result and media generation
is checked before returning data. Local pending writes are flushed after
filesystem detachment; the USB host cannot write either disk.

## Implementation and validation

The portable `UsbMscCrLib` owns SCSI, Bulk-Only Transport and reset recovery.
Each LUN has independent geometry, sense, media-error and eject state. The app
handles USB control requests, including GET_MAX_LUN, and adapts the core to
`EFI_USBFN_IO_PROTOCOL`. `UsbMscBlock.c` supplies the BlockIo backends. Type-C
negotiation remains in the existing platform driver.

Bulk transfers are serialized and use USB-provider transfer buffers. Exit
stops the controller, drains completions and releases buffers before restoring
local disk access. If USB retirement fails, potentially DMA-visible USB buffers
are retained and the app asks for a reboot. Disk callbacks are synchronous and
no longer run after the event loop exits, so disk ownership can still be released.

Host tests cover native/NT portable-core builds, both LUNs with different sector
sizes, media/error isolation, automatic selection and failure rollback, and the
actual application plus DWC3 path in DEBUG/RELEASE sanitizer builds. Physical
UFS/SD export still requires board testing. Save UART logs through app exit and
host enumeration messages. This exercises USB2 full/high-speed device mode;
SuperSpeed and Windows KDNET are separate bring-up tasks.

Private test USB identity: `1209:0001`; the serial is derived from the UFS path
and geometry. See the library README for protocol references and test details.

SPDX-License-Identifier: MIT
