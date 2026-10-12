# HDK8350 preserved display observation

This library reads a bounded inventory of current SM8350 DPU7, DSI0/1,
PHY0/1 and display clock registers. It has one callback, `Read32`, and no
write, delay, ownership-transfer, IRQ-acknowledgment or resource-enable path.
The UEFI caller must establish powered, mapped access before calling it.

`CrHdk8350DisplayReadInventory` makes two fixed sweeps, currently 1218 reads,
and records configuration stability. Dynamic frame/line counters and DSI0
video-engine status are excluded from the comparison. DSI command busy,
DSI1 activity, timing-engine enable and all selected configuration remain
part of that comparison. No retry loop or poll runs inside this library.

`CrHdk8350DisplayAdmitPreserved` admits only a stable, linear packed 32-bit
VIG0 or DMA0 at the base blend stage, through CTL0/LM0/INTF1 and four-lane
DSI0/DPHY0. It requires the board's 1920x1080 timing, source and output
geometry, nonsecure source, disabled source alpha, single plane, valid aligned stride, disabled
secondary DSI path, active resource gates and DSI0 clock parent selection.
Split, command, DSC, writeback, secondary CTL, multirect, scaled, decimated,
protected and nonlinear configurations are unsupported.

The returned `Device` is the inherited display IOVA. Admission does not
prove its CPU physical address. Before publishing GOP, the caller must
prove every byte of `Device..Device+Bytes` through all currently applicable
nonsecure SMMU stream routes into the retained board splash reservation,
and obtain a second admitted inventory after a bounded delay showing real
frame-count progress. Returned channel masks describe the observed unpack
order for `PixelBitMask`; they do not guess a BGRX layout.

The board's two supplied DTs describe the LT9611 family bridge as UXC, on
QUP SE15 at address 0x2b, fed by DSI0. This read-only core does not access
the bridge and does not claim chip identity, its MCU firmware version,
HDMI output or a cold start. A future FIFO bridge diagnostic should log
revision bytes and nonzero MCU firmware before UXC timing operations.

Source facts come from public Linux DPU7 catalog and DRM register code,
public SM8350 Android driver code and the supplied Android/Linux HDK DTs.
The focused runner independently compares the Linux block catalog against
Android DT offsets, then compiles the actual implementation with host
DEBUG ASan/UBSan and into an EFI ARM64 DEBUG object. It covers every read
failure, unstable configuration and route/geometry/format/resource rejection.
Those results establish source and compile behavior only.
