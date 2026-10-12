# Waipio resource and bus integration

MU's existing `PciHostBridgeDxe` owns PCI enumeration and root-bridge I/O.
Crane supplies `PciHostBridgeLib`, its private `PciSegmentLib` implementation,
and resource protocols. There is no `PcieCrDxe`. Other PCI consumers use MU's
`UefiPciSegmentLibPciRootBridgeIo`, so they cannot instantiate another copy of
the cold-initialization state.

Waipio's platform memory map provides device mappings for PARF/QMP and the
DBI, configuration, and translated I/O windows before cold initialization.
MU registers and maps BAR MMIO apertures while creating the root bridges.
The bridges report unassigned resources and combined prefetchable/nonprefetchable
windows so MU publishes resource allocation and its PCI bus driver assigns BARs.
The ARM CpuIo2 implementation accepts these translated CPU I/O addresses and
validates both the original and legacy-PCD-translated range for overflow.
Configuration accesses use native byte/halfword/dword transactions so a
command-register write cannot acknowledge adjacent W1C status bits. The
one-MiB configuration aperture uses per-BDF iATU targets with ECAM shift mode
disabled, matching Linux's non-ECAM DesignWare configuration path.

## Source and target scope

The implementation was compared with the local Linux checkout at
`40288c9206c17eb66a603262e06a58d300d0f279`, principally:

- `arch/arm64/boot/dts/qcom/sm8450.dtsi` and `sm8450-hdk.dts`;
- `drivers/pci/controller/dwc/pcie-qcom.c`;
- `drivers/phy/qualcomm/phy-qcom-qmp-pcie.c` and its QMP register headers;
- `drivers/clk/qcom/gcc-sm8450.c`, `clk-branch.c`, and `clk-rcg2.c`;
- `drivers/interconnect/qcom/sm8450.c` and `bcm-voter.c`;
- `drivers/soc/qcom/rpmh-rsc.c` and `drivers/regulator/qcom-rpmh-regulator.c`;
- `drivers/iommu/arm/arm-smmu/`.

Both Waipio build targets select `Target/Sm8450`. Supply identities and PCIe0's
Gen2 board limit come from the mainline HDK DTS. Mainline leaves PCIe1's
maximum link speed unset, but the vendor Waipio HDK DTB used by this firmware
sets `qcom,target-link-speed = <3>`, adds PCIe1 qref=`ldob5`, and selects the
vendor PHY overrides at absolute offsets `0x1174` and `0x140c`. The target
therefore follows the vendor DTB for this board while retaining the mainline
register and resource cross-check. The local Linux tree does not supply an
MTP board DTS. MTP currently shares this description and requires board
verification of its rails, GPIO wiring, and link limits.
PCIe0 also sequences the HDK WCN6855 PMU before PERST release (Phase 20).
This supplies endpoint power for PCIe discovery; Wi-Fi firmware and networking remain
separate from root-complex/endpoint power initialization.

The PCIe1 AUX source lifecycle is corroborated by public OnePlus SM8450
[`pci-msm.c`](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/pci/controller/pci-msm.c),
which selects the PHY AUX parent only after `pcie_phy_init()` and restores XO
before GDSC power-off. The same revision's `gcc-waipio.c` describes the ordinary
selector at `0x9d080`: PHY=0, XO=2. These public sources supply the sequencing
reference; historical executable evidence is not an implementation source.

## Cold initialization and ownership

`CrDALDxe` publishes device data before the resource drivers run. See
[CrDAL.md](CrDAL.md) for the provider and lifetime contract.

| Resource | Owner |
| --- | --- |
| CMD DB metadata | `CmdDBCrDxe` |
| LDO requests | Crane `RpmhCrDxe` direct CmdDB/RPMhLib votes |
| NoC bandwidth requests | `ICBCrDxe` and `ICBLib`, using the Linux SM8450 graph |
| RPMh TCS and BCM programming | Crane `RpmhLib` through CmdDB metadata |
| GDSC, GCC clocks and resets | Crane-owned `ClockCrDxe`/`ClockLib`; see [ClockOwnership.md](ClockOwnership.md) |
| PERST and wake GPIOs | `GpioCrDxe` |
| PCIe DMA streams and page tables | `SmmuCrDxe` / `SmmuLib` |
| QMP PHY, PARF, DBI and iATU | `PcieLib` called by `PciHostBridgeLib` |

For each root complex, initialization asserts PERST, applies the LDO voltage
and HPM mode before enabling the rails, enables its power domain, acquires
ICB paths, and attaches its SMMU streams. The complete controller clock group,
including PIPE, is enabled before the controller BCR is asserted and released,
matching Linux `qcom_pcie_init_2_7_0()`. PARF is configured before the PHY BCR
sequence and its non-PIPE clock votes. QMP power-control bits and register
tables precede the PHY's additional PIPE reference, PHY SW_RESET release, and
START. Only after PHY readiness, DBI/iATU/SID configuration,
and the required reset delay does initialization release PERST and start LTSSM.

PCIe1 explicitly selects XO through `ClockCrDxe.SetParent` before PHY reset
and AUX gate enable, switches to the PHY source after the readiness poll, and
restores XO before teardown can reset/gate the PHY or power down its domain.
A failed source switch aborts initialization; a failed restoration retains
the PHY's prerequisites for retry. Ordinary mux enable still preserves its
selector, and the AUX branch retains strict halt polling. The two successful
source switches are logged for board validation.

SM8450 also sets the PARF system clock/wake controls from the public Waipio
`msm_pcie_enable()` sequence (`0x365e`), while preserving unrelated fields
and clearing the PHY power-down mux. The refreshed log had `0x361c` on both
ports, missing `CORE_CLK_CGC_DIS` and `L1_CLK_RMV_DIS`. The host regression
requires these controls before PHY programming and LTSSM, starting from
both zero and the captured value with unrelated bits set. Startup logs the
result and CLKREQ override. PCI1 has an installed PCIe 5G network card, so
its timeout is a populated-slot failure; this correction still needs a board
boot to establish link training and downstream enumeration.

Crane owns the interconnect transaction. `ICBCrDxe` resolves each Linux BCM
name through `CmdDBCrDxe`, calculates the aggregated X/Y votes in `ICBLib`,
and submits grouped active-set commands through `RpmhLib`. This is separate
from the PMIC rail adapter, whose resource identities were verified in both
boards' DAL configuration; see
[WaipioPmicResources.md](WaipioPmicResources.md).

`ICBCrDxe` exposes the Linux provider-local path IDs used by the UFS, PCIe,
and MDP consumers:

| Crane path | Linux provider path |
| --- | --- |
| PCIe0 memory `0x1000 -> 0x1100` | `pcie-anoc MASTER_PCIE_0 -> mc-virt SLAVE_EBI1` |
| PCIe0 CPU/config `0x2000 -> 0x2100` | `gem-noc MASTER_APPSS_PROC -> config-noc SLAVE_PCIE_0` |
| PCIe1 memory `0x1001 -> 0x1101` | `pcie-anoc MASTER_PCIE_1 -> mc-virt SLAVE_EBI1` |
| PCIe1 CPU/config `0x2000 -> 0x2101` | `gem-noc MASTER_APPSS_PROC -> config-noc SLAVE_PCIE_1` |
| UFS memory `0x3000 -> 0x3001` | `aggre1-noc MASTER_UFS_MEM -> mc-virt SLAVE_EBI1` |
| UFS config `0x3002 -> 0x3003` | `gem-noc MASTER_APPSS_PROC -> config-noc SLAVE_UFS_MEM_CFG` |
| MDP memory `0x4000 -> 0x4001` | `mmss MASTER_MDP -> mc-virt SLAVE_EBI1` |
| MDP config `0x4002 -> 0x4003` | `gem-noc MASTER_APPSS_PROC -> config-noc SLAVE_DISPLAY_CFG` |

The Crane interconnect API follows Linux and expresses average and peak
bandwidth in kB/s, where one kB/s is exactly 1000 bytes/second. `ICBLib`
aggregates all active paths, sorts dirty BCMs by VCD, and writes complete
RPMh batches. A failed batch restores the previous path and BCM state before
returning, and release drives a zero vote before dropping the path.

Each port records acquired resources. Teardown fences the endpoint before
revoking SMMU mappings, then releases dependent resources. Shared rail/clock
references prevent one port's failure from dropping the other port's vote.
A failed release retains ownership and prerequisites for a later retry.
Voltage and mode requests are included in rollback even if rail enable was
never reached. An incomplete shared acquisition cannot satisfy another port.

Other firmware clients can retain SMMU resources. Crane preserves valid stream
matches and enabled or stream-bound context banks without resetting the
controller. The Linux/Android basis and ownership limits are documented in
[SmmuOwnership.md](SmmuOwnership.md).

## QUP, GPI, I2C and SPI

`QUPCrDxe` publishes Linux-derived wrapper/engine descriptions and arbitrates
each serial engine. `I2CCrDxe` runs HDK8450 I2C9 in GENI FIFO mode, while
`GpiCrDxe` drives the Qualcomm GPI-DMA engine for the same bus. Each transfer
holds the QUP lease, so FIFO and GPI cannot own the serial engine concurrently.
The LT9611 chip-ID probe exercises FIFO, GPI and then FIFO again through their
Crane protocols. All three chip-ID bytes must agree across the three reads;
a failed restore blocks MCU initialization and bridge publication. The extra
`LT9611 I2C FIFO restore test` line verifies that DMA ownership was released
and the engine can return to FIFO transfers; it is not a fallback from GPI.
Native FIFO distinguishes address NACK from read data-phase NACK using
GENI's GP_LENGTH, as Linux does. A read data NACK cannot hide other errors:
success still requires command completion and the complete requested data.
The FIFO sequence follows Linux `geni_i2c_fifo_xfer()`: intermediate commands
set STOP_STRETCH and the next command issues a repeated START. A combined
WRITE_READ opcode is not required for a register-address write followed by
a read.

GPI initializes its own control/event/error sources, IRQ selection and
scratch registers before allocating channels. It polls the source status
with CPU interrupt delivery masked; no OEM GPI setup or preceding FIFO
transfer is required for this configuration. CONFIG0 is submitted once at
the start of each request, matching Linux `geni_i2c_gpi_xfer()`. Read messages
queue RX DMA before a linked TX READ GO, and completion is checked against
the RX DMA TRE address and byte count. Controller errors and failed control
acknowledgements retain DMA memory and fence the SE. The host tests model
these conditions, ring wrap and repeated START; physical bus behavior still
requires board testing. Both transports currently require I2C sequencer
firmware already loaded by the early boot firmware (FW protocol 3); they
reject missing/wrong firmware and do not invoke an OEM DXE to load it.

The two DMA masters need separate SMMU mappings: GPI ring/event DMA uses
SID `0x56`, while GENI payload DMA uses QUP1 SID `0x43`. Linux
`geni_i2c_gpi()` maps payload through the serial engine's parent device,
independently of GPI's ring allocations. Both streams are attached before
allocation; each buffer is authorized only for its corresponding stream.
Tests cover complete initialization, failed second attachment, incorrect
wrapper metadata and payload-permission rollback. Ring START success alone
does not establish that payload DMA is authorized.

GPI enables `SE_DMA_IF_EN` at offset `0x2004` with a read-modify-write that
preserves other bits, before enabling DMA mode and GSI events. Its first
transfer snapshots include `dma-if-en` alongside the channel/event ring
contexts and pointers. The transport tests check this ordering through
actual read/write requests under the QUP lease, including an initially
disabled interface and preservation of unrelated register bits.

Before any channel START or transfer doorbell, GPI reads back each ring's
writable configuration, length and 64-bit base address. Event context is
validated before its initial doorbell. Hardware-owned state fields are
excluded. A mismatch aborts setup and uses the normal quiescence/retention
path. Global error enable is bit 0; general error enable remains bits 0-3,
matching public Linux `drivers/dma/qcom/gpi.c`. Tests inject bad context
readbacks for all three rings and require zero START/data doorbells.

Crane I2C endpoints use the native SE1 transport. There is currently no
Crane-owned native SPI transport: `SPICrDxe` is excluded from the Crane and
HDK8450 active manifests because its source is only a legacy adapter for the
private MU `SPIDxe` protocol. The standalone adapter test does not add that
driver to firmware and is not SPI hardware evidence.

After all three chip-ID reads, the LT9611UXC driver reads MCU firmware version
`B0:21`, following Linux's bridge probe. Version zero cannot provide the
MCU-managed video setup and is rejected before bridge publication. Its GPIO
interrupt only marks pending work; a one-shot timer at TPL_CALLBACK reads
`B0:22`/`B0:23` and acknowledges pending status under the MCU bank lock.
Video configuration shares that lock. Failed bus work stays pending for the
next timer tick. Each callback rearms the timer for 10 ms after its work
finishes: the 50 ms MCU unlock delay must not overrun a periodic timer and
keep DXE dispatching callbacks indefinitely. ExitBootServices stops the timer; the platform GIC driver's
exit handler disables interrupt delivery for the firmware handoff.

The native I2C adapter implements synchronous repeated START and completion
checks. Asynchronous event/callback requests return EFI_UNSUPPORTED.
The retained SPI adapter source supports the private backend's equal-length
full-duplex transaction format and reports unsupported standalone CS/clock
operations. It is kept only for source-level compatibility testing; active
Crane firmware has no SPI protocol dependency on the OEM driver. The public
Crane protocol prefixes remain compatible; extensions increase the revisions.
The private ABI declarations are in `EFIMuBusProtocol.h`.

## HDK8450 display and storage replacements

The active HDK8450 build selects `MDSSCrDxe` instead of `DisplayDxe` and
`UfsHcCrDxe` plus MU `UfsPassThruDxe` instead of `UFSDxe`. Check the active
DSC/FDF and build report when verifying this selection: binary import
manifests also contain entries which are not selected by the platform build.

`MDSSCrDxe` reads the packaged LT9611 panel XML, brings up the SM8450 DPU/DSI
resources, maps the framebuffer, and publishes one GOP mode. GOP, the DSI
protocol and the display Device Path are installed on the same handle. The
Device Path initializer is shared with Andromeda's preferred-console lookup;
GraphicsConsole requires that protocol before it can bind to the GOP handle.
Preferred-console selection resolves that exact Device Path and connects
the controller before requiring GOP: `GopOverrideDxe` hides the native GOP
until the rendering engine binds. For GOP on the controller itself, selection
duplicates its Device Path; the generic boot-manager helper supports only
GOP children ending in ACPI ADR nodes. Tests cover both forms, hidden GOP,
partial path rejection, connection failure and missing GOP.
Setting mode zero clears the framebuffer and publishes the writes to the
display engine. The BLLP and EOF-BLLP low-power XML settings independently
control their DSI register bits. Before publishing GOP, the driver enables
frame/line counters and requires enabled interface status, a new frame and
consumed CTL flush within 250 ms. Failure still owns scanout until shutdown
confirms DMA has stopped. This verifies interface progress, not monitor output.

MDSS requests only the GDSC-gated AHB leaf at 19.2 MHz; an additional zero-rate
non-GDSC AHB vote would conflict on their shared RCG. MDP and LUT share an
explicit 200 MHz request. Cleanup disables every domain-dependent clock,
including AHB, before releasing MDSS GDSC, then releases GCC access clocks.
Tests check the rates, shutdown ordering and dependency retention on an AHB
release failure.

The original `waipiolog.txt` ended immediately after the first GPI TX doorbell
and has no exception state. Context checks and host regressions do not
establish the crash cause. A fresh board boot must confirm PCIe1 AUX gate/PHY
readiness and endpoint enumeration, the GPI chip-ID/FIFO-restore sequence,
MDSS frame progress and visible HDMI output. PCIe0 link timeout remains
subject to the endpoint PMU scope described above.

The refreshed `waipiolog.txt` confirms LT9611 firmware `0x41`, successful
scanout setup at the corrected DPU addresses and GOP publication at
1920x1080. PCIe1's AUX source switches now succeed, but both root-port links
time out at LTSSM state zero. The capture still stops after the first GPI
TX doorbell write returns, with no exception registers. The dual-stream
DMA, preferred-console and PARF fixes address demonstrated software defects;
a fresh boot must verify GPI completion/FIFO restore, console binding and
PCI1 link/enumeration before attributing the crash or claiming hardware success.

The later `waipiolog2.txt` confirms MDSS AHB/GDSC, four display SMMU
attachments and the framebuffer mapping now succeed. It stops during the
LT9611 display handoff, before PCIe and the ReadyToBoot GPI self-test. There
is no exception dump. Its 37 FIFO acquisitions after the chip-ID line are
consistent with the firmware check, one IRQ drain and video timing setup;
they do not by themselves indicate a retry loop or identify the faulting PC.

That review found an incorrect DPU base: the mainline
`dpu_8_1_sm8450.h` catalog offsets are relative to the `mdp` resource at
`0x0AE01000` in `sm8450.dtsi`. Android's resource starts at `0x0AE00000`
and its block offsets are correspondingly `0x1000` larger. The native
driver now uses the mainline base consistently in programming, polling and
shutdown. The resulting physical addresses are:

| Block | Address |
| --- | --- |
| MDP top | `0x0AE01000` |
| CTL0 | `0x0AE16000` |
| DMA0 | `0x0AE25000` |
| DSI0 INTF1 | `0x0AE36000` |
| DSI1 INTF2 | `0x0AE37000` |
| LM0 | `0x0AE45000` |

Host tests constrain these addresses independently of production constants.
Bring-up stages use `DEBUG_INIT`, included in HDK's existing debug mask:
firmware check, bridge timing, DSI probe/reset, framebuffer clean, DPU
programming, scanout start/progress and GOP publication. A new board capture
must establish whether the address correction resolves the observed stop.

Video prefetch follows the public OnePlus Android display driver at
`877203a7cf9b79973d44a9db716490f95d820110`: `sde_hw_catalog.c` selects a
40-line prefill budget and a one-line fetch delay for Waipio, and
`sde_encoder_phys_vid.c` scales the budget above 60 Hz. The driver calculates
the counter from XML timing before host setup, writes `INTF_PROG_FETCH_START`
and enables `INTF_CONFIG[31]` before CTL flush. The HDK 2200x1125 total timing
selects counter 2472801. An enabled trigger must remain in vertical front
porch and before frame rollover; at least two lines must be selected to allow
the delay. With less than two front-porch lines programmable fetch is disabled.
Host tests cover the HDK and Truly video timing, higher refresh, one-line
deficits, short porches, and counter bounds; scanout still needs board testing.

`BorderColor` programs the DPU interface
border color and defaults to zero when absent. Equivalent decimal/hex scalar
duplicates are accepted; conflicting values, including conflicting corrected
and original vertical-polarity spellings, are rejected.

The parser accepts the original XML element names, including the shipped
`InvertVsyncPolairty` spelling. Parsing the 32 packaged files does not imply
that all of their modes can run. The current display path supports the
LT9611 single-controller DPHY RGB888 video configuration. DSC, CPHY,
command-mode scanout and split/dual controllers remain unsupported.
Known unsupported requests are rejected before resource or
controller setup. The parser owns the five original `DisplayResetInfo`
values and backlight type/PMIC type/bit-width fields, retaining explicit
presence separately from missing values. Private-source reference is limited
to the XML keys' meanings, formats, units and enums; see the
[XML key reference](WaipioDisplaySourceReview.md). Driver implementation must
use Linux/Android sources, without inspecting or copying other private code.
Commas and whitespace are accepted as reset-list separators. Generic native reset and
backlight execution are still not implemented. LT9611 uses the source-backed
Linux bridge reset sequence. Native panel supply/GPIO/backlight wiring is
separate board data. The current HDK8450 DXE dependency and initialization path
require the LT9611 bridge; accepting another panel's XML syntax does not
provide that native panel's resource lifecycle.

The resource and command layers can select one DSI0 or DSI1 bank, including
its PHY/PLL, clocks and distinct DISPCC parent selectors. Controller ownership
is retained through failed cleanup; a request for another controller cannot
reuse its command buffer or touch its registers. Android Waipio describes
both hosts using L6B at 1200 mV and L5B at 880 mV:
[waipio-sde.dtsi](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_s_12.1_10_pro/vendor/qcom/proprietary/display-devicetree/display/waipio-sde.dtsi)
and
[waipio-sde-common.dtsi](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_s_12.1_10_pro/vendor/qcom/proprietary/display-devicetree/display/waipio-sde-common.dtsi).
This does not add simultaneous dual display or change the HDK8450 LT9611
route: the bridge is wired to DSI0 and its mode validation enforces that route.

`DSIInitSequence` and `DSITermSequence` use owned descriptors copied from the
original XML rows. DCS and generic short/long writes execute through the
selected DSI host's command DMA in low-power mode, with video disabled.
A separate IOMMU common buffer has read-only access for the MDSS streams;
command data is flushed
before the software trigger. Completion and errors are polled with interrupt
sources masked. A timeout resets the command engine and checks the documented
busy bits before revoking its mapping. Failed fencing or revocation pins the
buffer and prerequisites for a cleanup retry.

The `ff` delay row is interpreted in milliseconds, corroborated by the
matching Truly FHD Android panel sleep-out/display-on sequence and Android's
`post_wait_ms` parser. XML rows are not interpreted as Android's seven-byte
packet headers. The maximum long-write payload is 4092 bytes; the full init
and term configuration is checked before any panel command or resource
setup. RX/ACK and compression/PPS packets remain unsupported. The shipped
LT9611 XML has no command sequence and does not exercise this new transport.

Initialization configures the host, runs the init sequence, then starts video.
An explicit image unload first removes the published protocols, stops scanout,
runs the term sequence and releases resources. An open console can refuse
protocol removal without losing its active display. ExitBootServices keeps
scanout running for the bootloader/OS framebuffer handoff. If initialization
fails and DMA/resource cleanup cannot finish, the image stays resident without
publishing GOP so its unload handler can retry cleanup without losing ownership.

The framebuffer consumer explicitly registers its mapping with the SMMU
provider for that handoff. Only read-only mappings whose complete backing page
range is GCD reserved memory qualify. ExitBootServices retains those page-table
entries and their reserved table storage, revokes every other mapping in the
same stream, and blocks all other streams. A failed revoke or poisoned context
blocks the affected stream instead of retaining unrelated DMA access. Starting
Unmap withdraws handoff preservation even if teardown needs a retry. DSI
command buffers never opt in. This prevents the SMMU exit handler from cutting
off the live GOP scanout; OS display/SMMU takeover still needs board validation.

`UfsHcCrDxe` supplies the standard UFS host-controller and platform protocols
for the non-PCI host. It owns platform resources, QMP PHY initialization and
SMMU mappings; MU owns UFS commands and exposes ExtScsiPassThru. The standard
`UfsPassThruDxe -> ScsiBusDxe -> ScsiDiskDxe` chain already provides BlockIo.
`UfsPciHcDxe` produces the same `EDKII_UFS_HOST_CONTROLLER_PROTOCOL`, but a
NonDiscoverable PCI wrapper would still need Qualcomm platform/PHY sequencing
and the current SMMU DMA ownership. Native `UfsHcCrDxe` remains that adapter.
LUN discovery starts each unit-descriptor
request with a fresh buffer and length, so an absent early LUN cannot hide a
later enabled LUN. Descriptor type and the returned/header lengths must cover
each consumed field before it affects the exposed LUN mask. Explicit invalid
descriptor index/IDN/selector responses can skip an absent unit; transport
errors abort discovery and enter fenced Start cleanup. This prevents a full
query timeout budget from being repeated across every LUN. Query stalls log
the query identity, OCS, interrupt/host/doorbell status and all five UIC error
registers before aborting the slot or disabling HCE. Device-init
polling includes query latency in its elapsed-time budget and accepts a cleared
flag on the final poll.

Query requests carry descriptor data inside the Query UPIU and have no PRDT.
Their UTRD direction is therefore `UfsNoData`, including READ_DESCRIPTOR and
WRITE_DESCRIPTOR; a packet's descriptor direction only controls UPIU payload
handling. The wire-format regression checks both operations and their response
offsets. The October 4 03:24 board log completed HS-G4 negotiation but timed
out on descriptor reads. This correction needs a fresh board boot before
descriptor access or storage enumeration can be reported as working.

HDK8450's device reset is the output-only TLMM pin 210. Linux
`sm8450.dtsi` maps TLMM at `0x0f100000`, and `pinctrl-sm8450.c` places
`UFS_RESET` at offset `0xde000`: control is `0x0f1de000`, output is
`0x0f1de004`. Crane expresses this as base `0x0f000000` plus special-pad
offset `0x1de000`. The GPIO library must apply that offset exactly once.
The former double addition accessed `0x0f3bc000` immediately after logging
GPIO 210 configuration. Its offset `0x3bc000` matches the September 27
CONFIG_NOC fatal log's ERRLOG2 value; the NoC address encoding itself has
not been independently decoded. The register-address bug is reproduced by
the host test, while recovery from the board crash still requires a boot test.
Missing mux/OE/input fields on this output-only pad are not zero-width
bitfields to shift; SDC control-only pads have no IO register to access.
UFS initialization now logs progress before and after reset, interconnect,
power and clock setup so a subsequent fatal access can be narrowed down.

Startup establishes the initial PWM link,
then negotiates HS Rate B before exposing LUNs. The selected gear is bounded
by the hardware PARAM0 limit (at most G4), local RX and peer RX capabilities;
the connected RX/TX lane reports must be symmetric, nonzero and no more than
the two QMP lanes implemented by this host. Cold PHY setup
applies Linux's base tables, G4 overlays where supported, then HS-B tuning
before starting the PHY. Device refclk, adaptation, termination and timeout
attributes precede the power-mode request. MU requires both UIC command
completion and the asynchronous power-mode completion with PWR_LOCAL status.
Failures abort initialization; they do not silently leave a claimed HS device
on PWM. AXI, ICE and UniPro boot at the documented 300 MHz maximum, using
GPLL0 MAIN / 2 and matching UTP/QUniPro timers (300 cycles/us, 12 cycles/40ns).
Linux initializes these clocks at maximum and lowers HS gear before scaling
them down; negotiating G4 while leaving them fixed at 75 MHz did not follow
that sequence. This correction still needs hardware validation.

HCE is disabled before retrying PHY setup. Start failure and driver Stop
cancel the timer and confirm HCE is disabled before releasing queued requests
or DMA lists. Failed cleanup retains state per controller for Start/Stop retry;
completion callbacks cannot recursively tear down that same retained owner.
Successful Map tokens remain owned by MU until Unmap succeeds; Crane never
silently destroys a consumer-visible token during a later resource call.
Mappings that fail before publication stay in Crane's pending cleanup list,
which must drain before their allocated buffers can be freed.

UFS transfer slots have software reservations in addition to hardware
doorbells. A completed request keeps its slot until response processing and
resource release finish, and the async timer cannot observe a request before
submission. The PRDT uses the same DWORD-rounded length as the DMA buffer;
the command UPIU retains the original byte count. Both completion paths check
the SCSI RESPONSE transaction type, residual and sense-data lengths before
copying device-supplied data. An OCS success with an empty or REJECT UPIU is
an error and does not publish read data.
Readiness NOPs and queries also require their expected NOP IN and QUERY RSP
transaction types before initialization or query output can succeed.
SCSI payloads use host-allocated staging even when the caller's address is
aligned. Successful reads reach the caller only after response validation and
successful data Unmap. A failed synchronous request detaches its caller before
retaining mappings and staging for later cleanup; delayed provider copyback
therefore cannot touch caller memory after the request returns.
Timeout cleanup waits for the selected doorbell to clear and escalates to
confirmed HCE disable if needed. A failed controller fence retains the DMA
resources and rejects new I/O; explicit driver Stop can retry recovery without
repeating the long recovery wait on every timer tick. Query and NOP requests
use the same retained ownership, while completed requests require no extra
hardware clear.

Focused host regressions include `run_mdp_panel_tests.py`,
`run_mdp_resource_map_tests.py`, `run_mdp_scanout_tests.py`,
`run_mdp_dsi_command_tests.py`, `run_mdp_gop_publish_tests.py`,
`run_geni_transport_tests.py`, `run_gpio_tests.py`, `run_ufs_tests.py`, and
`run_ufs_request_resources_tests.py`. The GOP publication regression invokes the production
entry point and GOP callbacks with mocked providers. It checks the shared
console path, mode queries, framebuffer clearing, ordered init/video/term,
unload refusal, retained-DMA cleanup retries and all four BLLP/EOF-BLLP
combinations. UFS regressions cover platform resource/HS sequencing, actual
MU UIC completion handling, and production Start/Stop cleanup with fault
injection. These tests do not execute BDS or
establish that a physical monitor displays pixels.

On HDK8450, `LT9611 I2C FIFO test`, `LT9611 I2C GPI test`, and
`LT9611 I2C FIFO restore test` must report successful, matching chip-ID reads.
The subsequent `MDSSCrDxe: XML panel` message
indicates GOP publication, which still needs visible console output to
validate scanout. `Crane UFS host protocol installed` only indicates host
publication; resource-ready and platform-phase messages help locate failures,
and `Crane UFS: HS-G... Rate B` reports completed power-mode negotiation.
Actual device enumeration and BlockIo reads establish storage operation.

## Validation boundary

The native QUP SE1 lease now acquires and votes core, config and memory ICC
paths before FIFO/GPI clock setup or MMIO. All three votes are held until
GENI and GPI are quiescent. Failed rollback/release retains the remaining
handles and fences the SE; failed quiescence retains every path. A later
FIFO/GPI owner must acquire a fresh complete set of votes. Other engines
without target ICC metadata are rejected, rather than silently borrowing
QUP1 resources. The memory vote covers the supported 1 MHz FIFO ceiling
and 400 kHz GPI mode.

QUP1 routing follows the public OnePlus Waipio wrapper DT at revision
`877203a7cf9b79973d44a9db716490f95d820110` (`waipio-qupv3.dtsi`):
core uses QUP_CORE_1, config uses A1NOC_SNOC to LLCC, and memory uses
aggre1 QUP_1 to EBI1. This agrees with the public kernel's `waipio.c` at
`449760504b3a75728eb60479f5b34dc51cb263ce`. The upstream SE DT's
aggre2/MASTER_QUP_1 pair resolves to QUP0 because provider-local IDs overlap;
Crane therefore uses the corroborated downstream QUP1 route. The QUP1 BCM
uses `vote_scale=1`, while ordinary bandwidth BCMs default to 1000.
The shared graph also uses Linux's SN2 membership at `qnm_aggre1_noc`,
SN0 keepalive, EBI ACV enable vote and MDP-only subset of MM1 membership.

Native GPI requires Crane SMMU attachment before DMA allocation and validates
required protocol revisions and callbacks. Its DEPEX waits for native
QUP/Clock/GPIO/IOMMU/SMMU services; FIFO also waits for GPIO. Neither
transport falls back to a packaged Qualcomm bus driver. UFS enables its
QMP qref clock before PHY calibration and reasserts core reset on a failed
deassert before releasing power resources.

On 2026-10-03, the complete native host suite passed, including SMEM parser,
SMEM DXE lock/duplicate handling, early SMMU detach, SMMU adapter, GENI, ICC,
UFS qref/reset ordering, PCIe, clock and MDSS tests. The fresh HDK8450
DEBUG/CLANGPDB build passed with 126 images validated. These are
source/host/build results; FIFO/GPI chip IDs, visible GOP scanout and UFS
enumeration still require a boot of this image on the board.

On 2026-10-05, all 25 native host runners passed. After adding the DMA
interface snapshot and strengthening the ordering/lease assertions, the
focused GENI transport tests passed again. The HDK8450 DEBUG/CLANGPDB build
completed with 123 PE images validated; its generated APRIORI GUID order
and FFS inclusion also passed the explicit build-directory audit.
The original `waipiolog.txt` recorded a successful FIFO LT9611 chip-ID read
(`17.04.92`), but MDSS AHB acquisition failed, PCIe1 PHY AUX timed out,
and the log ended after the first GPI TX doorbell with a crash marker.
The rebuilt image still needs board validation of GPI completion and FIFO
restore, GOP scanout, PCIe links and UFS enumeration.

On 2026-10-06, the focused GENI/GPI, SMMU, GOP, serial/graphics-console,
PCIe cold-init and PCI host suites passed. The rebuilt HDK8450 DEBUG/CLANGPDB
image includes the dual DMA streams, preferred-console connection and PARF
clock controls. Postbuild validation verified 123 images; generated FV
APRIORI order and FFS inclusion passed. The image SHA256 is
`2f21a0d05d96db642d3a0624e9e674f5d349788293dd8d02eb7146847180a36f`.
The existing Edk2-Pytools FDF-reader warning remains; the independent
generated-FV audit succeeded. No boot of this new image was performed here.

The subsequent 9057-line `waipiolog2.txt` confirms the FIFO/GPI/FIFO tests and
1920x1080 GOP publication succeed. UFS PWM queries complete, but descriptor
queries after HS-G4 time out repeatedly and end in a watchdog reset. PCI1 is
populated with a PCIe 5G network card; both ports still report LTSSM state 0
with ready PHYs. PARF clock controls already read back as 0x365e. The PCI GPIO
adapter now verifies the configured mux/OE/pull/drive/output latch and prints
the PERST, CLKREQ and WAKE readback. Input levels are diagnostic; an endpoint
may drive either level. Output latch readback does not establish physical
connector voltage or presence of the 100 MHz reference clock.

ConSplitter's virtual GOP has no DevicePath and must not count as a physical
display in `GopOverrideDxe`. The override checks existing and future GOPs;
SWM selects GOP and SRE on the same handle and watches both protocols. OSK
retries its controller when SWM publishes and uses that rendering GOP. The
graphics-console regression covers virtual plus physical GOPs, delayed
publication, ambiguity and override rollback. Actual SWM/OSK initialization
and UFS/PCI1 operation still require a boot of the rebuilt image.

The Phase 8 HDK8450 DEBUG/CLANGPDB build completed on 2026-10-06 at 11:37 +0800.
UFS, clock, graphics-console, GPIO, PCIe cold-init/host, serial ConIn, OSK IRQ
and GOP regressions passed with sanitizer coverage where applicable. Postbuild
validation verified 123 images, and the independent generated-FV/APRIORI audit
passed. The 1,683,456-byte image SHA256 is
`1d40a16bcfc4a26b6ae5b00221ace6bd8f8c77edcb418f144c5a67429ed619b0`;
the 4,194,304-byte FD SHA256 is
`b95067545c5acc5b0180493663baaf3dd9606ec4fdbf7f65141a29459b215f2e`.
The build log is `.tmp/waipio-continuation/build-log4-retry.log` in the enclosing
workspace. No board boot was performed by this session.

The refreshed 21,635-line `waipiolog.txt` supplied at 11:48 +0800 confirms
300 MHz UFS clocks, completed HS-G4 negotiation and BlockIo for LUNs 0-5
plus RPMB. Rendering/SWM and deferred OSK initialize, FIFO/GPI/FIFO tests
pass, and the boot reaches Shell without a captured reset or exception.
Both PCIe ports now stop at GPIO validation: native GpioSetConfig forced
output pull to none and ignored ordinary input drive strength. The library
now honors explicit pull/drive independently of direction, preserves
UNCHANGE fields and programs the output latch before OE/mux. UFS explicitly
clears its reset-pad pull; LT9611 preserves IRQ-pad drive. The PCI host test
now exercises real GPIO DXE/library/SM8450 target registers on both ports,
including stale state, latch ordering and rejected writes.

UFS does not support MODE SENSE(6), as documented by mainline ufshcd.c.
ScsiDisk write-protection detection now uses MODE SENSE(10) with DBD and
falls back to (6) only for an unsupported opcode. It uses aligned data/sense
buffers and validates transport status and returned mode headers. The
focused SCSI regression covers both formats, fixed/descriptor sense,
timeouts, malformed/short replies, allocation failures and cleanup. UFS
continues to use the native Qualcomm host-controller protocol followed by
UfsPassThruDxe, ScsiBusDxe and ScsiDiskDxe. The HDK8450 FV now also packages
source DiskIoDxe and PartitionDxe, completing the chain to the existing FAT
driver. These two generic drivers and GptLib were already in the common
DSC, but their disabled device FDF entries prevented partition enumeration.
No PCI emulation or additional APRIORI placement is needed for these layers.

HDK8450 disables the inherited low-resolution Shell request because current
LT9611 scanout only advertises 1920x1080. SMMU/SMEM successes no longer use
ERROR; repeated successful TLB/map messages are removed, and DXE DMA traces
use DEBUG_VERBOSE. Actual sync/readback/ownership failures retain ERROR
and fault snapshots. The supplied log contains no such SMMU fault report.
Internal-storage Not Found cannot be attributed to missing boot files until
the restored partition chain has been tested on the board. USB and PXE
boot handles are absent in this capture. The repeated C40000002/V03050007
events immediately before boot attempts match MU's nonfatal memory-type
allocation-change telemetry from BdsDxe and MsBootPolicy. Their severity
is EFI_ERROR_MINOR (MS_WHEA_ERROR_STATUS_TYPE_INFO); ASSERT reporting uses
EFI_ERROR_UNRECOVERED instead. No failure saving MemoryTypeInformation is
reported, and the capture does not include the telemetry page counts.
Source/host validation does not establish PCI1 link or 5G card operation;
the next board capture must show verified GPIOs followed by clocks, training
and endpoint enumeration.

The final Phase 9 HDK8450 DEBUG/CLANGPDB build completed at 12:47 +0800 on
2026-10-06. GPIO/PCI integration, SCSI, UFS, GENI/GPI, native bus/LT9611,
SMMU/SMEM, MDSS resource and graphics/serial-console suites passed.
Postbuild validation verified 123 images; the expanded generated-FV audit
confirmed APRIORI order and the complete native UFS/partition/FAT chain.
The 1,695,744-byte `Build/WaipioCrPkg/qcom-hdk8450.img` SHA256 is
`bca6f798854eea4bb3b6b1d9ccd643b2b42c6bf5675a80f07a692a294812534d`;
the 4,194,304-byte FD SHA256 is
`783d51f23cfdcdc9926237e2f33b54b7170fbb0b7072d742fef5379869350ca8`.
Logs are `.tmp/waipio-continuation/build-log5-final.log`,
`log5-manifest-final.log` and `log5-*.log` in the enclosing workspace.
The existing Edk2-Pytools FDF-reader warning remains; the independent FV
audit passes. No board boot was performed by this session.

The common Crane APRIORI includes only CrDAL, CmdDB and SMEM. HDK8450's active
device APRIORI explicitly orders RPMh, Clock, GPIO, ICB, SMMU, QUP, GPI and I2C
before MDSS and LT9611. Its console tail follows BDS, as on the reference
devices: SerialDxe, TerminalDxe, ConPlatformDxe, ConSplitterDxe and
GraphicsConsoleDxe. The MU host bridge and SPI adapter retain dependency
dispatch. RamPartitionDxe is omitted on HDK8450 because there is no native
producer for the RAM-partition protocol it consumes.

Validate the actual final build with
`python3 Platforms/CranePkg/Tests/check_hdk8450_source_manifest.py --build-dir Build/WaipioCrPkg/DEBUG_CLANGPDB`
(select the corresponding directory for a different build target). Without
`--build-dir`, the command audits source manifests only. It checks source
APRIORI entries against every generated GUID position and verifies their FFS
inclusion plus the complete native UFS/partition/FAT chain, so an old output
from another build cannot validate the final image.

Host tests compile the production library/adapter sources with EDK2 headers,
MMIO/protocol mocks, and address/undefined-behavior sanitizers. They cover
resource ordering, shared-resource rollback, ICB votes, SMMU programming,
CrDAL data contracts, bus transfer semantics, and interrupt callback lifetime.
Run the `Tests/run_*_tests.py` runners from the enclosing firmware workspace
with `TMPDIR=/var/tmp` if `/tmp` is constrained.

Crane's `RpmhLib`, `ClockLib`, and `ICBLib` own the corresponding hardware
transactions. Their MMIO tests cover bounded polling, command completion,
shared references, and vote computation; the interconnect tests exercise the
Linux UFS/MDP paths and rollback through `ICBCrDxe`.

Firmware builds validate compilation, linkage, dependency expressions, and
FFS packaging. They do not establish LT9611 chip-ID reads, PCIe link training,
endpoint operation, or hardware DMA isolation. Those require boot logs and
device tests on each board, including failure/retry and the HAL coexistence
checks above.

## Native UCSI and Type-C transport (Phase 10)

The refreshed 1,929-line waipiolog.txt confirms UFS GPT/FAT reads and
FIFO/GPI/FIFO completion. The user also confirms a real DSI/LT9611 picture.
PCI1 still times out at LTSSM 0 with an installed PCIe 5G card. UFS
MODE SENSE(10) still reports host=0x14/target=0x2 without sense data. Neither
remaining hardware error is established as fixed by this update.

HDK8450 now packages `UCSICrDxe` from source. It consumes host-scoped SMEM,
uses the ADSP GLink descriptor/TX/RX items 478/479/480 and IPCC LPASS kick,
and opens PMIC_RTR_ADSP_APPS. Its UCSI/PAN engine observes connection,
power/data roles, orientation, DP pin assignment/lane count and HPD/IRQ.
Role methods queue PD requests; only received connector status establishes
a negotiated role. The 20 ms relative monitor remains active through Shell
and is cancelled at ExitBootServices. It reports missing remote firmware
after five seconds without blocking boot. A transport or command fault
publishes a detached error state; PMIC command faults recover on a new
channel session rather than matching untagged late replies to new commands.

The scoped SMEM API preserves the legacy global API, serializes allocation
with the same hardware mutex and distinguishes a duplicate allocation from
success. GLink checks FIFO bounds, supports fragmented receive messages,
retains outgoing replies under backpressure and sends READ_NOTIF when
blocked. PMIC channel closure discards queued old-session traffic and
completes both CLOSE directions before reusing its local channel slot.
Session generations ensure a close/reopen in one poll resets UCSI state.

Native I2C publishes LT9611 instance 10 (QUP1 SE1, GPIO32/33) and FSA4480
instance 6 (QUP0 SE5, GPIO206/207). Each has independent clock/FIFO state.
QUP0 votes use QUP0 core (vote_scale=1), A2NOC/LLCC and DDR paths; QUP1
continues to use A1NOC. The QUP0 AHB and SE5 GCC nodes are operational, with
the SE5 19.2 MHz BI_TCXO parent and Linux vote masks. Publication rollback
retains driver storage/image
when an installed protocol cannot be removed.

`run_usb_typec_tests.py` exercises GLink wire handshake, FIFO wrap,
fragmentation, intents/backpressure, channel close/restart, malformed SMEM,
UCSI v1/v2 reset/ACK/role requests, PAN HPD/IRQ and timeout/overflow detach.
Its production DXE lifecycle suite covers setup and publication rollback,
timer setup/rearm failures, delayed remote availability, session reset and
ExitBootServices. ICB, GENI/GPI, native bus/LT9611 and scoped SMEM tests
cover the added QUP0/I2C5 dependencies. The manifest audit requires the
renamed UCSI driver in source and in the explicitly selected generated FV.

The initial transport checkpoint did not include FSA4480 switching, shared
USB/DP PHY ownership, DP AUX/EDID/link training/INTF0 scanout or DWC3.
The native consumers described below are now packaged from source. The
external DP/eDP XML files are configuration/override fixtures, not PHY
initialization recipes. The required connection is inserted after entering
Shell: USB-C to DP, then an unspecified DP-to-HDMI adapter and HDMI display.
Actual PD firmware availability and hardware hot-plug need a new board log;
the original capture contains no native USB/DP producer activity.

The checkpoint DEBUG/CLANGPDB build completed at the build log's
2026-10-06 13:51:19, with 124 images verified. The source/generated-FV
audit, UCSI transport/lifecycle, scoped SMEM, clock/target, ICB, GENI/GPI
and native bus/LT9611 sanitizer suites passed. The final build log is
`.tmp/waipio-continuation/build-log6-verified.log` in the enclosing workspace.
The 1,703,936-byte `Build/WaipioCrPkg/qcom-hdk8450.img` SHA256 is
`0a37dd2edd5c7d1d2edb34bbbd09d8cc9ebd54945fee9fc599eb33a16ba6d5e3`;
the 4,194,304-byte FD SHA256 is
`efe475b3e7482538f2c60d8d4b1bc782fe7dbab7d36ec9dff1a79d67187b0ff6`.
No board boot or flash was performed for this checkpoint.

## Native Type-C DP and DWC3 consumers (Phase 10)

`UsbDpPhyCrDxe` owns the SM8450 USB2 HS PHY, USB3/DP QMP combo PHY and
FSA4480 SBU mux on I2C instance 6. Its public Linux sm8350 USB3/DP tables
match the SM8450 binding. Separate USB and DP leases protect shared clocks,
rails, orientation and lane routing; topology changes wait until both
consumers release their leases. FSA4480 programming verifies readback and
uses the documented disable/select/enable delays. Optional RPMh supplies
resolve lazily through CmdDB so a missing USB rail does not prevent existing
display or PCI resource providers from publishing.

`MDSSDpCrDxe` monitors negotiated DP Alt Mode and HPD throughout Shell.
It reads actual AUX/DPCD and EDID data, trains the available lane/rate pairs,
handles HPD IRQ service and retries failed connections at bounded intervals.
It mirrors the native MDSS framebuffer through independent
DMA1/CTL1/LM2/PP2/INTF0 blocks; the DSI/LT9611 scanout remains the console
source. The framebuffer producer is pinned with OpenProtocol BY_DRIVER.
The four MDSS SIDs are attached and authorized for direct read-only DMA;
reserved-framebuffer mappings are preserved through ExitBootServices
regardless of callback order. Pixel-clock M/N programming and DP interface
divider ancestry are validated independently of the transport fixtures.

The supported DP subset is progressive SST RGB888 at the existing
framebuffer size, using the Linux legacy TU calculation with bounded
scanline buffering for fractional boundaries. EDID base/CTA timings
must advertise a matching mode. Scaling, interlaced modes, DSC and MST are
unsupported. The 1080p60 regression admits HBR with two lanes and HBR2 with
one lane as well as RBR with four lanes. The 1080p59.94 HBR/two-lane case is
covered independently. Blanking, active time and register limits must fit
the TU delay. Unsupported configurations return an error before scanout.
External-DP/eDP XML overrides are not substituted for the real sink data.
The unspecified DP-to-HDMI adapter must expose a working DP AUX receiver;
there is no native DP++ implementation or adapter-specific manual action.

`Dwc3CrDxe` follows the negotiated Type-C data role. Host mode registers
non-discoverable xHCI and connects the source NonDiscoverable PCI, XhciDxe,
UsbBusDxe, keyboard, mouse and mass-storage stack. Device mode provides
USB2 control/bulk EFI_USB_FUNCTION_IO_PROTOCOL, including setup/status,
address/configuration, endpoint stalls, bulk/ZLP transfers and bus-reset
recovery. Composite configurations validate unique interface numbers and
endpoints against hardware limits. A new SETUP interrupting DATA recovers
at status NRDY without cycling Run/Stop; unpowered Configure never reads
registers. It does not include a default gadget personality; a function
client must supply descriptors and its class behavior. PMIC PD firmware
owns CC, VBUS and negotiation; firmware role requests do not assume success
until connector status confirms the negotiated role.

DWC3 attaches the native SID 0 before DMA. A controller-handle identity
protocol validates its immutable MMIO/SID tuple instead of assigning a
synthetic PCI BDF to SMMU ownership. Non-discoverable PCI common-buffer
allocation/free now uses IoMmuLib and retains provider WC memory. Failed
authorization preserves the original Map error, successful rollback clears
the token, and failed rollback/revoke/free retains ownership for retry.
Role transitions halt the core and disconnect consumers before releasing
the shared PHY. Device transfers use cache-line-isolated endpoint TRBs;
ENDTRANSFER completion and reset retirement are asynchronous. Partial
buffer-map failures fault the function before the buffer can be reused.
ExitBootServices halts USB DMA while retaining allocations for SMMU fencing.

DP transport/scanout/lifecycle, DWC3 core/function/role, PCI DMA,
UCSI transport/lifecycle, shared PHY, clock/target, ICB, RPMh, SMMU identity
and MDSS preservation sanitizer checks passed. The Phase 10 DEBUG/CLANGPDB
build completed at the build log's 2026-10-06 15:17:36 with 132 images
verified. Source/generated-FV audit and scoped whitespace checks passed.
The existing Edk2-Pytools FDF-reader warning remains; the separate generated
FV audit passes. Logs in the enclosing workspace include
`.tmp/waipio-continuation/build-log6-dp-usb-verified.log`,
`log6-manifest-dp-usb-verified.log` and `log6-dwc3-verified.log`.

That Phase 10 `Build/WaipioCrPkg/qcom-hdk8450.img` was 1,769,472 bytes with SHA256
`ad768026e147b0bcee168419c602580c2d53f5a885a0917bcc5ceda48e80968c`.
The 4,194,304-byte `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd` SHA256 is
`ebb3eec04252719313663a92ae8941acc7ef3c8d671545ee7452d5720f94dea9`.

No board boot or flash was performed for this image. The supplied log
predates these native consumers and cannot establish remote PD readiness,
DP-to-HDMI compatibility, link training, USB enumeration or role switching.
Capture the boot and insertion after Shell, including UCSICr, UsbDpPhyCr,
MDSSDpCr and Dwc3Cr diagnostics, plus disconnect/reconnect and both plug
orientations. Existing PCI1 LTSSM 0 and UFS MODE SENSE failures remain
unresolved; this update does not establish a hardware fix for either.

## Completion audit and board-test image (Phase 11)

The current waipiolog.txt is still the 2026-10-06 13:08:22 capture and
contains no native Type-C DP or DWC3 run. The audit fixed independently
reproduced DP TU admission, USB unpowered Configure, composite endpoint
validation and EP0 SETUP_PENDING recovery defects described above.

UFS now classifies transport completion by OCS/UPIU type and retains valid
SCSI target status and sense data. Sense copying validates the data segment
and truncates to the host's 18-byte prefix and caller capacity. Unsupported
EHS, malformed segments, residual counts beyond the request and overflow
still reject data copyback; valid sense survives a residual error. Residual
counts are honored even when the device omits the underflow flag, matching
Linux. Error diagnostics include CDB opcode, LUN/tag, OCS, response type,
Response/SCSI status, flags, EHS, segment/sense lengths and residual/expected
byte counts. The existing log does not identify which response field caused
MODE SENSE to fail, so its board-level cause remains unproven.

DP, DWC3/role/PCI DMA, the full UFS suite, SCSI write-protection and UFS
request-release ASan/UBSan regressions passed. Sync and async UFS cases cover
long sense declarations, small client buffers and invalid wire responses;
USB covers both control data directions and an interrupted pending status.
The DEBUG/CLANGPDB build completed at 2026-10-06 15:44:00, with 132 images
verified. The selected generated-FV/source audit and scoped whitespace
checks passed; driver/entry/build references consistently use UCSICrDxe.

The latest `Build/WaipioCrPkg/qcom-hdk8450.img` is 1,769,472 bytes with SHA256
`01021ada2457e4ca6aadbbe6351a0f9a9a82d0704c235c6b22879d47fab954de`.
The 4,194,304-byte `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd` SHA256 is
`244a019033de10242c3268db7d0d32541daa5a673bade645822de0db496aa641`.
Logs are under `.tmp/waipio-continuation`: `build-log7-final.log`,
`log7-manifest-final.log`, `log7-dp-fixed.log`, `log7-dwc3-final.log`,
`log7-ufs-fixed.log`, `log7-ufs-final.log`, `log7-scsi-fixed.log` and
`log7-ufs-resources-fixed.log`.

No board boot or flash was performed. The implementation is ready for the
next board test: capture boot, insertion after Shell, disconnect/reconnect
and both Type-C orientations through the user's DP-to-HDMI chain. Verify
USB host enumeration and negotiated role changes; device mode requires a
UsbFunctionIo client because no default gadget personality is installed.
Check the new UFS raw response diagnostic if MODE SENSE still fails. PCI1
with the installed 5G card still requires LTSSM/power/refclk evidence.

## USB capture and native ADSP PIL (Phases 12-13)

The refreshed `waipiolog.txt` still identifies a 05:02:19 Oct 6 2026 build
and has no native UCSI, shared USB/DP PHY, DWC3, XhciDxe or UsbBusDxe startup.
DEBUG_INIT is enabled in this configuration, and both Secure Boot and NoSb
source manifests already include those consumers. The user boots with
`fastboot boot qcom-hdk8450.img`, connects a USB hub, and confirms UEFI does
not load ADSP. The native UCSI/PAN transport needs ADSP's PMIC GLink service;
without it, negotiated Type-C role and hot-plug notifications cannot arrive.

The native `PILCrDxe` and `PILCrLib` now load ADSP for HDK8450. References
include the user's [Linux SM8450 DTS](https://github.com/sunflower2333/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450.dtsi)
and [HDK DTS](https://github.com/sunflower2333/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450-hdk.dts),
[OnePlus Android kernel](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450)
and [OnePlus modules/devicetree](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450).
The inspected OnePlus branch is `oneplus/sm8450_v_15.0.0_oneplus_10_pro`.
Relevant Android sources are `drivers/remoteproc/qcom_q6v5_pas.c`,
`qcom_q6v5.c`, `drivers/soc/qcom/mdt_loader.c`, `smp2p.c`, `qcom_aoss.c`
and `drivers/firmware/qcom_scm.c`, with `waipio.dtsi`, `waipio-smp2p.dtsi`,
`waipio-pcie.dtsi` and HDK regulator/wiring descriptions in
`kernel_platform/qcom/proprietary/devicetree/qcom/`. Mainline remoteproc,
SCM SMC, MDT, AOSS, SMP2P, RPMh power-domain and interconnect implementations
provide independent ABI/resource checks. The Android `uses_elf64` flag
selects the coredump format; its MDT loader still decodes ELF32. The native
decoder handles ELF32 and ELF64 with explicit bounds and segment checks.

Firmware discovery follows this board's existing `uefipil.cfg` FULL_ADSP
entry: an internal, nonremovable UFS FAT volume with GPT Microsoft Basic
Data type GUID, `\image\adsp.mdt` plus the required `adsp.bNN` files. A packed
`\image\adsp.mbn` is tried when MDT is absent. All split segments come from
the same volume as the header. Driver binding selects the first qualifying
volume with a loadable image; this implements the GUID-based OEM policy,
without interpreting Android slot metadata. No firmware blob is embedded
or written to disk. Missing firmware returns Not Found and permits a later
connection attempt; malformed or unauthenticated images fail with stage
diagnostics. The firmware must be signed for this board's secure firmware.

DriverBinding.Start runs at TPL_APPLICATION, because FAT/UFS synchronous
reads can wait for completions. There is no filesystem I/O in a timer or
protocol-notify callback. BDS `EfiBootManagerConnectAll` connects the new
consumer to discovered FAT partitions. The CrDAL manifest owns immutable
PIL target data: PAS1, reserved `0x85e00000`/`0x02100000`, ADSP host2,
SMP2P SMEM443/429 and LPASS IPCC signal2. The loader validates the containing
reserved GCD descriptor, relocates loadable segments inside the carveout,
zeros BSS and authenticates with PAS init_image/mem_setup/auth_and_reset.
SCM convention probing supports 64-bit and 32-bit calls and bounded
interrupted/busy retries. Signed metadata uses page-aligned reserved memory
below 4 GiB, mapped normal noncacheable (WC on Arm). It stays allocated until
authentication or secure shutdown succeeds; failed attribute restoration
keeps the reserved pages for a later retry.

Startup sends AOSS `load_state` on, holds LCX/LMX ARC and XO active votes,
and acquires the Android crypto-to-DDR ICB route through CE0, SN3, SN0, SH0,
MC0 and ACV. CmdDB supplies resource addresses and supported ARC corner
indices. Prerequisites are held through Shell and released after PAS
shutdown. SMP2P polling negotiates versions/features, acknowledges restart
completion, checks fatal/ready state and signals the stop bit. Unload and
ExitBootServices stop only ADSP that this driver started. A pre-existing
ready SMP2P peer with no fatal flag is treated as borrowed and receives no
PAS shutdown.
That ready bit alone is not a liveness probe; GLink negotiation must still
complete before Type-C consumers can use the PMIC service. There is no
automatic remote-crash recovery or QRTR/service-registry implementation.

ExitBootServices performs bounded stop/PAS/QMP/RPMh/ICB operations with no
allocation, filesystem access, attribute changes or frees. Failed secure
shutdown retains ownership and prerequisites, and any pending metadata is
already reserved against OS allocation. UCSI checks the native PIL status
before GLink negotiation, clears cached USB state when PIL becomes
unavailable, and rebinds when it returns. It refreshes protocol discovery
before calls so unloading PIL cannot leave a callable stale pointer.

Both PCIe ports in this capture still time out at LTSSM state0 despite PHY
ready and deasserted PERST. OnePlus's PCIe1 data agrees with GPIO97/98/99,
Gen3 cap, `ldoh2` 912mV, `ldob6` 1200mV and qref `ldob5` 880mV; this audit
does not demonstrate an alternative programming sequence that fixes the
link. Added bounded DWC LTSSM transitions, DEBUG0/1, LinkCtl/LinkCtl2 and
ELBI/PARF PHY/PM/CLKREQ readbacks for the next boot. PCIe0's wireless PMU
sequence remains outside root-complex bring-up. PCIe1 with the installed
5G card is still unresolved and requires the new capture.

Independent ASan/UBSan fixtures cover ELF/metadata/carveout bounds, split
and monolithic payload/BSS, PAS ABI/retries, SMP2P/QMP, proxy votes, crypto
path release, partial/short/oversized file reads, missing firmware retries,
startup rollback, shutdown failure retention, borrowed ownership, unload
retry and allocation-free exit. PIL core/DXE, UCSI transport/lifecycle,
ICB library/DXE, CrDAL, RPMh and SMEM library/DXE pass. Phase12 PCIe, PCI host,
DWC3/PCI DMA, shared PHY and clock suites also passed before PIL integration.
The final integrated DEBUG/CLANGPDB `-s 0` build completed at 19:45:04 on
2026-10-06, using `.venv/bin/activate` and the requested
`./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0`. All 133 postbuild images
passed validation. The selected source/generated-FV audit verifies PIL and
the native USB stack, and compile commands confirm `SECURE_BOOT=0`.
The Android v3 boot image's kernel matches `bootpayload.bin`, whose gzip
payload contains this exact FD after the 112-byte boot shim.

`Build/WaipioCrPkg/qcom-hdk8450.img` is 1,708,032 bytes, SHA256
`8d12def88a301b067e1147e55d504144b6e574c7df432a5d9df0cb805b73e392`.
`Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd` is 4,194,304 bytes, SHA256
`628faa46df5fa7a1b4dd1637a7a2e676954388aa54e2fa6f109a1e12b8a8fc33`.
Logs are under `.tmp/waipio-continuation`: `build-log9-debug-nosb-reviewed.log`,
`log9-manifest-final.log`, `log9-pil-final.log`, `log9-typec-final.log`,
`log9-icb-fixed.log`, `log9-crdal-final.log`, `log9-rpmh-final.log`,
`log9-smem-final.log` and `log9-smem-dxe-final.log`. Source whitespace checks
pass. The uncompressed FV is sized automatically; the 4MiB compressed FD
has 2,441,304 bytes free.

No board boot or flash has been performed. On the next boot, capture
`PILCr:` discovery/authentication/readiness, `UCSICr:` router/UCSI startup,
`Dwc3Cr:` role and host enumeration, and PCIe's new link diagnostics. Also
capture hub insertion and removal after reaching Shell. Signed firmware
availability, PMIC service startup, hub hot-plug and PCIe training remain
hardware validation requirements.

## ADSP capability query and USB VBUS (Phase 14)

The new `waipiolog2.txt` saved at 19:58:27 on 2026-10-06 contains 1,962
lines and confirms native PIL, UCSI, shared PHY, DWC3 and USB stack dispatch.
PIL discovers `\image\adsp.mdt` on `modem_a`: 5,884 bytes, 26 program headers,
relocatable, with a 33MiB span matching the reserved carveout. Every attempt
then fails at `PAS support` with Unsupported, before proxy votes, signed
PAS init/auth, ADSP ready or PMIC GLink negotiation. This explains why the
current native role monitor produces no USB insertion event. The user's
measured absent VBUS is consistent with the missing PMIC/Type-C startup;
the capture does not establish a separate VBUS hardware failure.

The [Linux ADSP driver](https://github.com/sunflower2333/linux/blob/master/drivers/remoteproc/qcom_q6v5_pas.c)
and [OnePlus Android ADSP driver](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/remoteproc/qcom_q6v5_pas.c)
go through signed PAS startup without requiring `PAS_IS_SUPPORTED`. In the
user's Linux tree, `devm_qcom_pas_context_alloc` also does not call the
support helper. Native PIL had added a mandatory legacy command-7 gate that
these boot paths do not have. Its raw reply is absent from the old capture,
so Unsupported cannot distinguish a transport failure, a zero support reply
or a noncanonical true value. Do not infer the exact TrustZone reply.

`PILCrLib` now checks command-7 availability with the owner/SVC/command ID
`0x02000207` and records any legacy support reply as a diagnostic. Availability
and support Booleans accept any nonzero result. Signed `init_image`,
`mem_setup` when relocatable, `auth_and_reset`, and nonfatal SMP2P readiness
still determine startup success. An actual authentication rejection cannot
publish ADSP readiness. PAS ownership and shutdown/rollback prerequisites
are retained as before. This is limited to the native PAS1 ADSP target.

The two kernel SCM implementations agree on FAST convention probes and
standard PAS calls; no change to call flags is justified. Probes use
`0xc2000601` then `0x82000601`; PAS uses `0x420002xx` or `0x020002xx`, and
availability query arguments omit FAST/SMC64 flags. ArmSmcLib returns x6,
so interrupted-call cookie handling already matches Qualcomm's quirk.
SCM mapping now treats -1 as Device Error, -4 as Unsupported, -2/-3 as
Invalid Parameter and -5 as Out of Resources. New diagnostics print the
chosen convention and each completed call's FID, arginfo and raw x0-x3,
including errors. Busy/interrupted retries remain bounded and log limits.

PIL core/DXE ASan/UBSan fixtures pass with absent/rejected/zero/noncanonical
legacy replies in both conventions, actual signed-auth rejection, transport
error mapping, convention failure and busy/interrupted timeouts. UCSI
transport/lifecycle tests pass. Source/generated-FV audits pass, including
native PIL and USB drivers. The requested virtualenv build
`./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` completed at **20:12:21 on
2026-10-06**, 16 seconds, with all 133 images validated. The actual PIL.c
compile command uses `SECURE_BOOT=0`, and its linked PE contains the new
diagnostics. Android v3 kernel bytes match the current bootpayload.bin,
which decompresses to the exact 112-byte BootShim and current FD.

Current `Build/WaipioCrPkg/qcom-hdk8450.img`: 1,708,032 bytes, SHA256
`a0a1c4b41fdfc8f851083b1351965273302f6e52666d346bd8f79f15a16b5dad`.
Current `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4,194,304 bytes,
SHA256 `b2e620c5e02da00f5d2846c49906e46b660cd6304692256706d29cb8c75b23ba`.
This supersedes the Phase13 artifact. Logs are in `.tmp/waipio-continuation`:
`build-log10-debug-nosb.log`, `log10-pil.log`, `log10-typec.log`,
`log10-manifest-final.log` and `log10-artifacts.json`.

No board boot or flash was performed. The next capture must show signed
PAS startup and `ADSP PAS1 authenticated and ready`, then UCSI PMIC router
open, Type-C source/host state and DWC3 host enumeration. If it fails, the
new raw SCM results and exact later stage identify the next blocker. Hub
insertion/removal and VBUS remain hardware checks.

Both PCIe ports still time out at LTSSM0 in this capture. Port0 DWC DEBUG0
is `0x6e6d00`, port1 `0xc04800`, DEBUG1 is `0x8200000` on both; LinkCtl2
matches Gen2/Gen3 caps, ELBI control is 0, PARF PHY/PM/CLKREQ readbacks are
`0x900`/0/`0x10`. Ready PHY and deasserted PERST do not establish the cause
of receiver-detection failure. No new PCIe programming change is justified
by these readbacks. UFS now supplies valid Unit Attention 6/29/2 for MODE
SENSE; firmware discovery and FAT filesystem access nevertheless succeed.

## UCSI reset and remaining PCIe receiver detection (Phase 15)

The refreshed `waipiolog.txt` saved at 20:24:58 on 2026-10-06 has 1,969
lines. Signed PAS1 init, memory setup and authentication all succeed,
followed by `ADSP PAS1 authenticated and ready; UEFI owns shutdown` at
line 1649. The legacy PAS support reply is transport success with service
result zero, confirming why the former command-7 gate was inappropriate.
The user reports VBUS is now present. The PMIC GLink channel opens at
line 1719, but `stage=1 command=0x1` times out at line 1797. Native UCSI
then faults permanently and discards subsequent PMIC messages. This
explains the missing later Type-C logs; the capture has insufficient wire
diagnostics to distinguish UCSI reset completion, its transport reply or
the concurrently pending PAN enable acknowledgement.

Both [Linux UCSI reset](https://github.com/sunflower2333/linux/blob/master/drivers/usb/typec/ucsi/ucsi.c)
and [OnePlus Android UCSI reset](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/usb/typec/ucsi/ucsi.c)
clear an already-set Reset Complete with notification-enable zero before
issuing RESET, and resend RESET when the PPM still reports another
operation. Native UCSI now follows those steps with a fixed five-second
reset deadline. A preliminary clear-loop timeout with valid transport
permits the actual reset, including a valid read delivered before the
deadline timer consumes it. Missing or failed transport replies still
fault. Reset completion must contain only the Reset Complete bit.

The [Linux PMIC transport](https://github.com/sunflower2333/linux/blob/master/drivers/usb/typec/ucsi/ucsi_glink.c)
and [Android PMIC transport](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/usb/typec/ucsi/ucsi_glink.c)
dispatch replies by owner and opcode without requiring the request header
type. Native callbacks now accept either defined request or notification
type while retaining owner, opcode, pending-request, version and length
validation. PAN enable/ACK timeout or ACK-queue exhaustion clears Alt Mode
state and logs its own error; it preserves negotiated USB connector and
role state and allows UCSI to keep monitoring. This matches the kernels'
independent UCSI and Alt Mode clients. PAN failure still disables DP and
its negotiated SuperSpeed state for that session.

New startup diagnostics print the first 16 PMIC envelopes, UCSI version,
control commands and initialization CCI transitions. UCSI failure logs
include the precise operation, pending reads/writes, accepted reply counts,
reset writes, notifications and last envelope. The DXE failure snapshot
also prints GLink queued frames, free/busy intents and FIFO tail/head
positions. These distinguish missing service replies from stale CCI or
transport backpressure in the next capture.

PCIe still fails on both ports in this capture. PCI0 enters receiver-detect
state 1 at 118ms and 665ms, returning to state 0 one millisecond later;
PCI1 does so at 877ms. Neither reaches link-up within one second. PHY
ready bits, released PERST, CLKREQ pad configuration, controller reset
delays and the HDK supply tuples agree with the source references. Those
readbacks do not establish external endpoint power or a physical refclk.

The full PHY table audit found a concrete PCI0 transcription error:
RC `RX_MODE_00_HIGH2`, `RX_MODE_00_HIGH3` and `RX_MODE_10_HIGH4` had been
written at relative RX offsets `0x178`, `0x17c`, `0x190`. The
[Linux QSERDES v5 definitions](https://github.com/sunflower2333/linux/blob/master/drivers/phy/qualcomm/phy-qcom-qmp-qserdes-txrx-v5.h)
and [OnePlus Waipio PHY sequence](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/kernel_platform/qcom/proprietary/devicetree/qcom/waipio-pcie.dtsi)
agree on `0x164`, `0x168`, `0x194`, with values `0xbf`, `0x3f`, `0x38`.
The corrected writes preserve the separate mode-01 values `0x5c`/`0x34`
and mode-10 HIGH3 value `0x34`. This correction applies to PCI0; it is
not evidence that the PCI1 5G card is fixed. All 76 PCI0 and 100 PCI1
entries now match the Linux configuration with the intentional Android
PCI1 `CORE_CLK_EN=0x60` and `OSC_DTCT_CONFIG1=0x1d` overrides retained.
The requested user Linux fork agrees with the selected mainline tables.

On link timeout, PCIe now reads back the final PHY configuration of both
lanes, accounting for BASE entries intentionally overridden by RC. It
prints up to eight mismatches and a total. This is a read-only diagnostic
and does not change link speed, timing or reset policy. `mismatches=0`
establishes that the programmed configuration read back correctly, not
that the receiver, endpoint supply or refclk is working.

ASan/UBSan UCSI wire/lifecycle fixtures cover stale Reset Complete clear,
other-operation/error CCI resends, a fixed reset deadline, clear-loop
deadline arrival, missing replies, both PMIC header types and PAN timeout
followed by hub detach and computer-role reporting. PCIe cold-init fixtures
assert independent physical register addresses before LTSSM starts and
detect dropped/corrupted configuration writes. The new physical-offset
fixture rejects the old PCI0 target. PCI host/GPIO/resource, DWC3/role/DMA
and DP core/lifecycle sanitizer suites pass.

No board boot or flash was performed. The next capture must establish
UCSI startup and actual connector attach/detach, DWC3 host enumeration,
and PCI1 link behavior with the new PHY configuration readback. PCI0's
WiFi endpoint also has a separate PMU/power sequence outside the current
root-complex implementation.

The reviewed virtualenv build
`./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` completed at **20:52:02 on
2026-10-06**, 20 seconds, with all 133 images validated. UCSI's final
compile and the PCIe target/core compilation use `SECURE_BOOT=0`. Linked
UCSI and PCI host PE images contain the new diagnostics. Source/generated
FV inclusion and order checks pass. Android v3 kernel bytes match the
current bootpayload.bin and decompress to the exact 112-byte BootShim and
current FD. Source and documentation whitespace checks pass.

Current `Build/WaipioCrPkg/qcom-hdk8450.img`: 1,708,032 bytes, SHA256
`2e4b87e4a11c48d0aac33955be89fa3c7974cb44a19a8664511ba5c82632ccbc`.
Current `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4,194,304 bytes,
SHA256 `48065811175bf1b4ba389eb8287afe4787c0e2ea6dd2ebbcc6d713cfaf7658bb`.
This supersedes the Phase14 artifact and Phase15's initial 20:48:46 build.
Logs in `.tmp/waipio-continuation`: `build-log11-debug-nosb-reviewed.log`,
`log11-typec-final.log`, `log11-pcie-cold.log`, `log11-pci-host.log`,
`log11-dwc3.log`, `log11-dp.log`, `log11-pcie-source-audit.json`,
`log11-manifest-final.log` and `log11-artifacts.json`.


## 2026-10-06: HDK supply dependencies and pre-negotiation PDPHY power

The 20:59 `waipiolog2.txt` confirms the corrected PHY register writes:
PCI0 checks 76 registers and PCI1 checks 147, both with zero mismatches.
Both links still remain in receiver detection. ADSP PAS startup succeeds,
and the PMIC router returns valid UCSI version/read/write replies. RESET is
acknowledged but all 36 reads return CCI=0 with no notifications before the
five-second command deadline. This is a PPM completion stall despite working
transport; another change to the already matching CONTROL wire offset is
unsupported by this capture.

The endpoint is now an NVMe SSD in the onboard M.2 slot. The user confirms
S5701-6/-7 OFF and a 12V barrel adapter. Schematic LD20-33589 revision A,
sheets 52/64, shows external DC-fed 3.3V and VPH-fed BOB assembly options;
there is no demonstrated TLMM enable for the DC buck. GPIO71 reaches the
modem full-card-power-off pin, which is NC in the SSD pin legend on sheet 63.
It is not an NVMe VCC enable. S10B enables the external BOB and supplies
selectable slot IO, so PCIe1 now acquires S10B 1800mV/AUTO before its PHY rails.
The fitted external power path and voltage at J6302 still require measurement.

The [mainline HDK DTS](https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450-hdk.dts)
and [RPMh regulator driver](https://github.com/torvalds/linux/blob/master/drivers/regulator/qcom-rpmh-regulator.c)
identify these native input dependencies:

| Crane child | Crane parent | Requested parent voltage/mode |
| --- | --- | --- |
| L1B | S11B | 968mV / AUTO=6 |
| L6B, L9B | S12B | 1352mV / AUTO=6 |
| L2B, L5B, L7B, L1C, L2H | PM8350C BOB | 3296mV / AUTO=6 |

The SMPS values use valid 8mV selectors within the mainline limits; 3296mV is
[Android's BOB default](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/kernel_platform/qcom/proprietary/devicetree/qcom/waipio-regulators.dtsi)
within the mainline range. The external M.2 BOB is distinct from PM8350C BOB.
CrDAL publishes this topology, and RPMh acquires one input lease per active
child before programming that child. Shared inputs survive until their last
child/direct consumer releases them. Failed rollback retains input ownership;
invalid/cyclic topology and incomplete shared acquisitions are rejected.

Schematic sheets 23/33 connect L2B to PM8350BH VDD_PDPHY/VDD_VCONN. Previously
only USB PHY startup acquired L2B, after successful UCSI role detection. UCSI
now owns L2B before protocol publication/negotiation, with its BOB input
acquired first. Its startup rollback, unload and ExitBootServices release only
this reference. Failed withdrawal retains a stopped image and exit event for
retry. USB and DP rediscover Type-C before polling, so a removed UCSI image
cannot leave a cached callable pointer. These correct supply ownership and
ordering; neither the cold SSD nor CCI=0 proves the missing votes are the
only hardware cause.

Independent ASan/UBSan fixtures check numeric parent-before-child commands,
sibling/direct-parent sharing, all programming/withdrawal failure points,
invalid topology, L2B before negotiation, retained cleanup and UCSI provider
unload/reload. PCI cold, PCI host/GPIO/resources, USB/DP PHY, DWC3/DMA, DP,
CrDAL, MDP resource and UFS suites pass. Source manifest audit passes. The
reproducible source comparison is `.tmp/waipio-continuation/log12-power-source-audit.py`
and its JSON report. The requested virtualenv DEBUG -s 0 build succeeded at
21:50:20 +0800 in 17 seconds, with all 133 images validated. Generated-FV
inclusion/order, actual SECURE_BOOT=0 compile commands for all seven changed
native source units, linked diagnostics and exact Android v3 BootShim+FD
packaging pass. No board boot or flash was performed.

Historical Phase 16 artifacts (superseded by Phase 17 below):

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1708032 bytes; SHA256 `d3a10bf9e46baee17342255dca7b80b64fb4ddab58d14f528c6d444b6d67ad32`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `1a83e780a7f73fab35695644822a4ea012c98d488e63281e1d6b484bbd523b47`.


## Phase 17: IPCRTR services and persistent receiver detection

The refreshed `waipiolog.txt` (2026-10-06 22:05:04, 111848 bytes) includes
Phase 16's L2B prerequisite and parent-rail programming. ADSP PAS startup,
PMIC router traffic, display/console and UFS HS-G4 work. UCSI RESET is
acknowledged, but all 36 following reads return CCI=0 with no notifications.
PCIe0 briefly reaches Detect.Active; PCIe1 remains Detect.Quiet despite clean
PHY table readback. The current PCIe1 endpoint is an NVMe SSD in the onboard
M.2 slot. A cold SSD is not a supply measurement; 3.3V and receiver-detect
prerequisites must exist before the PCIe link can train.

Native GLink previously acknowledged `IPCRTR` without giving that channel
receive intents or preserving its remote intents. It now gives PMIC and
QRTR separate bounded receive pools and callbacks. QRTR implements the v1
and v3 headers, HELLO/service advertisement, receive flow control, SM8450's
PD mapper and a correlated `charger_pd` listener. The reference contracts are
Linux [QRTR](https://github.com/torvalds/linux/tree/master/net/qrtr),
[PD mapping](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/qcom_pd_mapper.c),
[PDR](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/pdr_interface.c)
and [QMI wire descriptions](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/qcom_pdr_msg.c).
The mapper advertises service 0x40/version 0x101 and returns the Linux
SM8350/SM8450 domain locations. The listener tracks service 0x42/version 1,
instance 74, path `msm/adsp/charger_pd`. It acknowledges state indications
independently of the UCSI command engine, so a previous RESET timeout cannot
block a PD restart. Local transactions are not reused across channel resets.

Known PD DOWN or IPCRTR loss withdraws connector readiness; a later UP
restarts the faulted UCSI state machine. Firmware without a notifier retains
the Android RPMSG startup path used by
[OnePlus UCSI](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/usb/typec/ucsi/ucsi_glink.c).
Once observed, the notifier remains required across close/reopen. Queues are
bounded, drained before readvertisement, and stale PDR requests/ACKs are
removed on endpoint loss. A blocked QRTR reply retains the final GLink RX
fragment until its reply/flow acknowledgement can be owned. Malformed QMI
still receives QRTR flow acknowledgement; malformed transport packets are
counted and discarded. This repairs a missing service contract, but the old
capture does not prove that it caused the PPM's zero CCI.

The UFS write-protection probe now retries valid UNIT ATTENTION on the same
MODE SENSE opcode, with at most three attempts per opcode. This follows
[Linux scsi_mode_sense](https://github.com/torvalds/linux/blob/master/drivers/scsi/scsi_lib.c)
and addresses the captured 6/29/2 sense. Opcode fallback remains limited to
valid unsupported-command replies; host failures, truncated sense and invalid
response headers do not trigger retry or fallback.

PCIe setup comparisons did not establish another register defect. In
particular, Linux's CPU-domain DBI/ATU/slave-aperture programming agrees with
native PARF, and CLKREQ input override is bit1 rather than the logged bit4.
An optional target diagnostic now reads GCC vote, branch, mux, RCG, BCR and
GDSC registers only after a link timeout, using offsets from
[gcc-sm8450.c](https://github.com/torvalds/linux/blob/master/drivers/clk/qcom/gcc-sm8450.c).
It makes the actual clock/reset state after PHY startup visible without
changing the controller's votes or timing. It does not measure the clock at
the M.2 connector, reset translation, slot voltage or fitted assembly options.

Host ASan/UBSan fixtures check both QRTR wire versions, instance packing,
mapper pagination and nested string encoding, PDR reply correlation,
DOWN/UP and close/reopen, unrelated/empty deletion handling, 64 remote GLink intents, independent PMIC/QRTR
channels, fragment backpressure, queue-full readvertisement, registration
timeouts and malformed packet bounds. UCSI monitor fixtures verify recovery
from a prior fault and known-DOWN fencing. The new SCSI fixture fails on the
saved pre-change source and passes with bounded retry. PCIe fixtures require
23 independently specified GCC addresses to be read while the port is powered
and PERST released, and prohibit GCC writes from diagnostics.

Hardware verification still requires this rebuilt image: look for `GLink
channel IPCRTR`, `IPCRTR open`, `PD mapper query`, `PDR listen`, and
`charger_pd state` before assessing UCSI completion and USB/DP hotplug. For
PCIe1, retain the new `link timeout GCC-...` lines together with PHY readback
and LTSSM transitions. No board boot or flash was performed in this phase.

The requested virtualenv DEBUG `-s 0` build succeeded at 22:49:47 +0800
(18 seconds compilation, 20 seconds overall), validating all 133 images.
Generated-FV inclusion/order, actual SECURE_BOOT=0 commands for GLink, QRTR,
UCSI, PCIe, target data and ScsiDisk, linked native diagnostics, and exact
Android v3 gzip BootShim+FD packaging pass. Historical Phase 17 artifacts
(superseded by Phase 18 below):

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1712128 bytes; SHA256 `5a5c38aaecd0dd510d3abdf616bd4e20f113f6d9a126593b1b09b7631a02cc87`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `5a2ce14d2db3f95fd88dcf2a5bcf905d1563fd05c949e5fd9c72b5f32a01aa27`.


## Phase 18: PAN survives UCSI failure and ChargerPD firmware logs

The new `waipiolog2.txt` (2026-10-06 22:50:56 +0800, 114597 bytes,
2027 lines) confirms the Phase 17 native service path: QRTR is online,
the notifier is registered, and `charger_pd` is known UP. UFS Unit Attention
retries succeed. UCSI RESET still gets an acknowledged write and 35 CCI=0
reads without notifications. Missing PDR readiness is therefore no longer
an explanation for this capture. Display, console, GPI and UFS startup
continue into Shell without a captured CPU exception or watchdog reset.

The PMIC receiver previously discarded every message after a UCSI fault,
including independent PAN notifications and PAN ACKs. Its timer also stopped
PAN processing. These paths now retain PAN events and acknowledge them after
UCSI failure, and log later UCSI notifications. Alt Mode data is kept
separately; USB/DP consumers still require valid connected UCSI status before
using it. This follows the separate clients in
[Linux PMIC Alt Mode](https://github.com/torvalds/linux/blob/master/drivers/soc/qcom/pmic_glink_altmode.c)
and [OnePlus UCSI](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/usb/typec/ucsi/ucsi_glink.c).

A completion timeout between fully acknowledged transport reads allows two
retries, separated by a one-second cooldown. Retry starts with a fresh version
probe and PPM reset while preserving PAN service state. Missing transport
replies, invalid payloads and other errors remain terminal: the wire replies
have no transaction IDs, so a late reply cannot safely identify a subsequent
request. This bounded native recovery policy differs from Linux's
`ucsi_init_work()`, which retries only EPROBE_DEFER. It does not establish
why the board's PPM remains at zero CCI.

Failure also triggers a read-only ChargerPD log capture using the existing
`PMIC_LOGS_ADSP_APPS` channel. The wire descriptions come from
[OnePlus charger-ulog-glink.c](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/soc/qcom/charger-ulog-glink.c)
and [Linux pmic_pdcharger_ulog.c](https://github.com/sunflower2333/linux/blob/master/drivers/soc/qcom/pmic_pdcharger_ulog.c).
The owner is 32778, init/runtime read opcodes are 0x23/0x18, requests are
16 bytes and replies contain a 12-byte envelope plus 8192 bytes of text.
A separate 8204-byte receive intent supports full and fragmented replies,
FIFO wrapping and backpressure without enlarging PMIC/QRTR message limits.
At most eight init pages and one runtime page are requested per channel
session. Intent acquisition and response waits are bounded to one second;
unsupported init reads fall back to one runtime read. No log properties are
written. Each timer prints at most four 160-character segments, with control
characters filtered and firmware text passed as data. Diagnostic failure
cannot stop UCSI/PAN or QRTR.

PCIe1's new GCC snapshot shows shared vote0=0x7fe83d7f, PIPE/AUX muxes=0,
reference gate=1, branch halt bits clear, BCRs=0 and GDSC=0xf822f000.
The PHY is ready and all 147 configuration readbacks agree; LTSSM nevertheless
remains Detect.Quiet. The controller sequence, mux/vote definitions and PHY
configuration were compared again with Linux and Android, without finding a
new demonstrated PCIe code defect. These register values do not measure the
M.2 3.3V rail, translated PERST# or the differential reference clock at the
slot. Those signals remain unmeasured; cold SSD temperature does not settle
the cause. No new PCIe link repair is claimed in this phase.

The PAN-after-fault fixture fails against the saved implementation and passes
after repair. ASan/UBSan fixtures also verify retained PAN state through a
successful delayed UCSI startup, exactly two retries, no missing-ACK retries,
8204-byte GLink framing/reassembly/isolation, bounded firmware log pages,
stale/malformed reply rejection and safe serial output. Type-C/QRTR/lifecycle,
DWC3, DP, USB/DP PHY, PCIe cold-init and PCI host suites pass.

Hardware validation for this image should retain `PAN port=`, `UCSI
notification`, completion-retry, `PPM snapshot`, `ChargerPD ... log` and
`ChargerPD:` lines. No board boot or flash was performed. UCSI completion,
USB/DP hotplug and PCIe1 link training remain hardware validation items.

The requested virtualenv DEBUG `-s 0` build succeeded at 2026-10-06
23:20:40 +0800 (17 seconds compilation), with all 133 images verified.
Generated-FV native/storage inclusion and order, SECURE_BOOT=0 compiler
commands for all four changed source units, linked PAN/retry/ULOG diagnostics,
and exact Android v3 compressed-kernel/BootShim+FD packaging pass. Current
Historical Phase 18 artifacts, superseded by the Phase 19 image below:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1716224 bytes; SHA256 `3f1c20998693be69a0b9a6443341656c5e87c9ab9c076b012d7b0761298366a7`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `29a804e88c80dea3993cb425be405d233e84297bff4270ef84a69e359e62be49`.

## Refreshed capture: independent UCSI deadlines (Phase 19)

The refreshed `waipiolog2.txt` (2026-10-06 23:52:39 +0800, 137735 bytes,
2182 lines) confirms the Phase 18 firmware-log path. ChargerPD returns two
init pages (8053 and 5132 bytes), an empty init page, and an 8034-byte runtime
page. The recorded runtime text is still early battery/platform initialization;
it does not identify the stalled UCSI RESET. Optional SPMI/peripheral warnings
alone do not establish the Type-C failure. There is no captured CPU exception.

UCSI again reports version 0x100 and zero CCI. This time its five-second
command deadline fires in OpReadWait with a transport read outstanding and
88 bytes already waiting in the RX FIFO. The code treated the command deadline
as that read's transport deadline, permanently faulting the client before the
read could return and preventing the bounded completion retry. An independent
near-deadline read regression reproduces this behavior on the saved source.

[Linux ucsi_reset_ppm](https://github.com/sunflower2333/linux/blob/master/drivers/usb/typec/ucsi/ucsi.c)
and [OnePlus UCSI transport](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/usb/typec/ucsi/ucsi_glink.c)
give blocking transport requests their own response interval; PPM completion
timing begins after the write transport acknowledgment. The native state
machine now mirrors these boundaries: each queued read/write has a separate
five-second response deadline, already-delivered replies are consumed first,
and the command completion interval starts after its initial write ACK.
RESET resends preserve that interval. Command ACK acquisition, transport and
CCI completion have separate bounded intervals. Genuine missing transport
replies remain terminal; no new request can reuse their untagged response.
Completed zero-CCI reads can now reach the existing two completion-only retries.

The read-only ChargerPD reader now continues after its first runtime page,
including empty replies, to expose later firmware records. It makes at most
32 runtime requests, at least one second apart, within a 60-second request
window. Its eight-init-page limit, one-second intent/response waits, wire bounds,
control-character filtering and four 160-character output segments per timer
are retained. A missing runtime reply ends capture rather than issuing another
indistinguishable request. Runtime window/page limits produce explicit messages.

PCIe1 now briefly reaches Detect.Active at 653 ms, returning to Detect.Quiet
one millisecond later; it never reaches Polling or L0. The shared clock votes,
reference gate, reset state, PERST sequencing, 1.8V S10B vote and 147 PHY
readbacks still agree with the reviewed Linux/Android configuration. This
transition does not measure M.2 voltage, translated PERST# or the differential
reference clock at the slot. This phase does not change PCIe register setup
or establish that link training is repaired.

The deadline regressions cover a pending read across command expiry, a reply
already delivered at expiry, late zero CCI followed by safe retry, full timeout
for a genuinely absent reply, delayed write ACK and normal status/ACK completion
at their deadlines. Firmware-log regressions cover later runtime pages, empty
pages, all 32 requests, one-second spacing, the request window, full 8192-byte
pages and retained output bounds. ASan/UBSan Type-C/ChargerPD/QRTR/lifecycle,
DWC3, DP, shared USB/DP PHY and PCIe cold-init suites pass.

For the next board capture, retain `PAN notifications enabled`, `PAN port=`,
`timeout scope=`, completion-retry, `PPM snapshot` and later `ChargerPD:` lines.
No board boot or flash was performed; UCSI zero CCI and PCIe1 receiver detection
remain unresolved on hardware.

The requested virtualenv DEBUG `-s 0` build succeeded at 2026-10-07
00:06:49 +0800 (18 seconds overall), with all 133 images verified. Final
generated-FV inclusion/order, SECURE_BOOT=0 compilation of PmicGlink.c,
ChargerLog.c and the header-dependent UCSICrDxe.c, linked diagnostic markers,
and Android v3 compressed-kernel packaging checks pass. The decompressed
payload is exactly the current 112-byte BootShim plus the current FD.
Historical Phase 19 artifacts, superseded by the Phase 20 build below:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1716224 bytes; SHA256 `6b5bfbb812246bd58ad5ae4e9c70b530546fb722d054aa6531059bf38b64f67e`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `91f8db290d91a3ab63d5f3ee6f2a6dada7fccc5abfa45f15bae8e0077e52b37a`.

## Populated PCIe0 Wi-Fi and slow PPM reset (Phase 20)

The user confirmed the HDK's PCIe0 Wi-Fi endpoint. The
[HDK mainline DTS](https://github.com/sunflower2333/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450-hdk.dts)
and local schematic PDF pages 65/66 identify a WCN6855, WLAN_EN=GPIO80,
BT_EN=81, SWCTRL=82 and XO=204. Controller/PHY initialization previously
left the endpoint PMU unsequenced. The target now publishes a separate
endpoint-power description and the native host adapter runs it after PHY
readiness, with PERST asserted and the SMMU attached, before link training.

Ten PMU supply aliases collapse to five physical RPMh inputs:

| Native resource | Vote, mV | PMU inputs |
| --- | ---: | --- |
| `smpb10` | 1800 | VDDIO |
| `smpb11` | 968 | VDDAON, VDDPMUCX, VDDRFA0P95 |
| `smpb12` | 1352 | VDDPMU, VDDRFA1P3, VDDPCIE1P3 |
| `smpe2` | 852 | VDDPMUMX |
| `smpc1` | 1856 | VDDRFA1P9, VDDPCIE1P9 |

All five use SMPS AUTO=6. S11B/S12B match the existing USB/UFS/PCIe parent
votes and mainline limits rounded up to 8mV selectors. S2E/S1C use the board
defaults from
[OnePlus Waipio regulators](https://github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/kernel_platform/qcom/proprietary/devicetree/qcom/waipio-regulators.dtsi),
within mainline constraints. These are PMU inputs, not the internal PMU LDO
outputs, and WCN's VDDPCIE names refer to the Wi-Fi device on PCIe0.

The sequencer follows
[Linux WCN power sequencing](https://github.com/torvalds/linux/blob/master/drivers/power/sequencing/pwrseq-qcom-wcn.c)
and [Android CNSS2 power](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/net/wireless/cnss2/power.c):
vote supplies, preserve WLAN's initial level while configuring its 16mA pad,
then for cold power-on wait 100ms, assert XO, wait 100us, assert WLAN, wait
2ms, release XO and wait 50ms before exposing the endpoint. The 100ms guard
covers Android's BT-off requirement when the inherited BT edge timestamp
is unknown. An already-high WLAN enable is not pulsed low. UEFI owns WLAN
after acquisition and stops it during shutdown; BT_EN and SWCTRL are read
only, never reconfigured or driven. SWCTRL is diagnostic feedback and is
not used as a readiness predicate. GPIO latch/mux/pull/drive readback is
mandatory for WLAN/XO.

Partial regulator/GPIO failures remain owned for rollback. Shutdown first
clears LTSSM, asserts PERST and blocks the SMMU, then deasserts WLAN/XO and
withdraws endpoint references before root/PHY resources. A failed release
retains state and power prerequisites for retry. Native RPMh parent counts
and the host's shared S10B reference preserve other live native consumers.

The refreshed `waipiolog.txt` (2026-10-07 00:17:01 +0800, 366708 bytes,
4183 lines, SHA256 `fa9dcbe3c30e38e1ba29e882ce48327d835d1380fd1cd84102297f8ffe9b2d78`)
contains the Phase 19 deadlines and later ChargerPD pages. Both failures are
completion timeouts with no missing transport reply and drained RX FIFO.
ADSP reports `PPM Reset timeout` followed by CCI=0x08000000 after the first
native five-second interval. The recovery clear command completes, but its
CCI remains set during repeated RESET writes. Later firmware pages show
real Type-C/PD negotiation and connector-status updates. Native USB/DP
still cannot start because UCSI has not reached ready state.

[Linux UCSI](https://github.com/sunflower2333/linux/blob/master/drivers/usb/typec/ucsi/ucsi.c)
uses a ten-second PPM completion interval and waits 20ms after RESET before
polling. The native engine now does the same and retains the separate
five-second GLink request/reply interval. RESET resends do not extend the
completion deadline; missing untagged replies remain terminal. A regression
with reset completion after 5.5 seconds fails against the prior engine and
passes after this fix. It also checks the first 20ms polling guard.

PCIe1 remains in receiver detection with only root bridges listed by Shell
`pci`. The new capture never reaches Polling/L0 on either port. No change
to PCIe1 PHY/register setup is justified by this capture. Slot voltage,
translated PERST and differential reference clock remain unmeasured.
The WCN endpoint fix does not establish NVMe power or link operation.

ASan/UBSan tests cover WCN timing/inherited WLAN, untouched BT/SWCTRL,
shared S10B/S11B/S12B votes through the production RPMh provider, every
regulator acquire/withdraw step, partial GPIO failures, dropped readbacks,
failed shutdown retry, native WLAN80/XO204 16mA register programming and
endpoint-before-PERST/LTSSM ordering. PCI host/cold-init, RPMh, UCSI/QRTR/
ChargerPD/lifecycle, DWC3, DP and shared USB/DP PHY suites pass.

The next board capture should include `WCN6855 power-on`, GPIO80/204,
`WCN6855 endpoint power ready`, `PPM RESET accepted`, Type-C ready state,
PAN and both PCIe training outcomes. No board boot or flash was performed;
PCIe0 discovery, UCSI startup/USB/DP hotplug and PCIe1 remain hardware
validation items.

The requested `source .venv/bin/activate` followed by
`./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` succeeded at 2026-10-07
00:39:51 +0800; all 133 images were verified. Generated-FV native/storage
inclusion and order, SECURE_BOOT=0 compilation of all four changed source
units, nine linked EFI copies for UCSICrDxe/PciHostBridgeDxe/CrDALDxe and
the new WCN/PPM markers pass. The Android v3 compressed kernel matches
`bootpayload.bin`; decompression exactly matches the current 112-byte
BootShim plus FD. Build audit is in `.tmp/waipio-continuation/log16-artifacts.json`.
Historical Phase 20 artifacts, superseded by the Phase 21 build below:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1716224 bytes; SHA256 `3e17f40274b11182ae856a3acc877468ac18392708c125b7d5eda1726e5346d8`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `f06979df3af8b3691b99952087198b660455ea30b9ab1da1d69e73847f01a62c`.

## Ready UCSI, USB clock conflicts and FSA arbitration loss (Phase 21)

The refreshed `waipiolog2.txt` (2026-10-07 00:57:23 +0800, 564397 bytes,
9750 lines, SHA256 `e3f9f517d45e4a633c67cc3e54b0842b48c8abb576b9ac084fe5157d44a39e82`)
reaches UCSI `ready=1`, connected host/source state, reverse orientation,
DP pin D with two lanes plus USB3, and HPD high. PAN notifications carry
subsequent HPD IRQs; UCSI commands and their acknowledgements complete.
The signed ADSP PAS sequence is still successful. The former UCSI reset
stall is absent from this capture.

USB role startup now exposes a native `gcc_usb30_prim_master_clk` enable
failure with `Access Denied`. The configuration, master and aggregate
leaves share the same master RCG in both the native target and
[Linux GCC SM8450](https://github.com/sunflower2333/linux/blob/master/drivers/clk/qcom/gcc-sm8450.c).
Previously the first leaf acquired its parent at rate zero, preventing
the master leaf from requesting 200MHz while the parent was owned. All
three leaves now request 200MHz. The read-only UTMI post-divider is
checked for divide-by-one while its leaf is gated, before clock enables;
the prior check after enable violated the clock provider's ownership
contract. The provider's shared-rate and enabled-divider guards remain
in force.

DP startup reports FSA FIFO IRQ `0x2009`, including GENI GP IRQ4.
[Linux GENI I2C](https://github.com/sunflower2333/linux/blob/master/drivers/i2c/busses/i2c-qcom-geni.c)
defines this as arbitration loss (an undriveable clock line), not address
NACK. The capture then repeats failed safe-state cleanup 821 times.
Native GPIO mux setup was missing the physical EGPIO handoff implemented
by both
[Linux pinctrl](https://github.com/sunflower2333/linux/blob/master/drivers/pinctrl/qcom/pinctrl-msm.c)
and [OnePlus Android pinctrl](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/oneplus/sm8450_v_15.0.0_oneplus_10_pro/drivers/pinctrl/qcom/pinctrl-msm.c).
When control bit11 advertises EGPIO capability, selecting a local mux
must set bit12 to claim AP ownership. Local mux readback alone can appear
correct while the physical pad remains remote-owned. The capture does
not contain these raw ownership bits, so it does not prove that this
omission caused the observed arbitration loss.

SM8450 now declares EGPIO's special remote selector (function 9). Explicit
local function selection claims capable pads; explicit remote selection
releases ownership while preserving the local mux. Unchanged-function
and pad-only edits preserve ownership, and non-capable/special UFS/SDC
pads retain their existing behavior. GetConfig reports the remote
selector if AP ownership is absent. This makes the PCI adapter's strict
readback detect a missing physical handoff, including WLAN80/XO204 and
capable PCIe control pads. QUP I2C now also verifies its function and OE
readback before issuing a command, rejecting a dropped ownership write.
The output latch is still written before enabling the pad.

FSA transactions request and verify 100kHz before each read/write,
including cleanup, matching the
[HDK DTS](https://github.com/sunflower2333/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450-hdk.dts).
The shared bus can therefore recover from another client's frequency
selection. Address `0x42`, QUP5 and GPIO206/207 agree with the DTS and
schematic; they are unchanged. Clock-selection errors propagate before
I2C commands, and retained cleanup keeps its power prerequisites. FIFO
errors now include instance, slave address and GP length as well as IRQ
and GENI status.

USB/DP monitors wait about one second between attempts to release
retained failed cleanup. Direct unload and ExitBootServices shutdown
paths remain immediate. Startup backoff is reset by actual route/role
changes rather than every UCSI sequence increment, so HPD IRQ traffic
cannot continuously restart a failed role. WCN endpoint power, FSA ID,
common PHY, USB role and DP link/mirror success markers use a level
visible in this platform's captured debug configuration.

PCIe0 still reaches Detect.Active briefly at 118ms and returns to Quiet;
PCIe1 does the same at 653ms. Neither reaches Polling/L0. PHY tables have
76/147 reads with zero mismatches. The general EGPIO correction is
relevant to Wi-Fi XO and any capable control pads, but this capture does
not establish that it repairs either link. PCIe PHY, DBI/iATU and clock
mux programming is unchanged. M.2 slot voltage, translated PERST# and
differential reference clock remain unmeasured; a cold controller is
not a measurement of its supply.

GPIO ownership and USB native-clock regressions fail against the saved
pre-fix production code and pass after repair. ASan/UBSan tests embed the
real GPIO/SM8450 target, native QUP FIFO path and real SM8450 clock graph.
They cover local/remote/unchanged ownership, dropped ownership writes,
PCI control/WLAN/XO latch ordering, all USB clock votes and gated divider
validation, FSA frequency failures and retained resources, one-second
cleanup spacing and HPD-independent startup backoff. GPIO, QUP/GENI,
USB/DP PHY, DWC3, DP, PCI host/cold-init, clock, native MU bus, UCSI/QRTR/
ChargerPD and CrDAL suites pass. The source manifest check also passes.

The next board capture should retain UCSI ready state, PAN port/IRQ,
EGPIO ownership handoff, WCN endpoint power ready, FSA ID/100kHz, common
PHY mode, USB role ready, DP receiver/mirror and both PCIe results. No
board boot or flash was performed; USB/DP operation and both PCIe links
require testing with the rebuilt image.

The requested virtualenv DEBUG `-s 0` build succeeded at 2026-10-07
01:23:20 +0800 (19 seconds overall); all 133 images were verified.
Generated-FV native/storage inclusion and APRIORI order, SECURE_BOOT=0
compilation of all eight changed C units, 21 linked EFI copies for the
seven affected modules, and linked diagnostics pass. The Android v3
compressed kernel exactly matches `bootpayload.bin`, and decompression
exactly matches the current 112-byte BootShim plus FD. All scoped source
and capture hashes stayed unchanged through the build. Audits are saved
in `.tmp/waipio-continuation/log17-artifacts.json` and
`log17-build-inputs.json`. Phase 21 artifacts, superseded by Phase 22 below:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1716224 bytes; SHA256 `0ab8d0ae73c4a5c06213e68ff9702dcd6bbe9ba10cac024c36af2c06a492b157`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `33caf44f68cf112475d99bb5a82ead8a304b03dfb694a58dccd9106c7231d30b`.

## Log18 unplug cleanup, reusable UCSI and PCIe enumeration

The refreshed `waipiolog.txt` (2026-10-07 01:31:18 +0800, 188348 bytes,
2829 lines, SHA256
`9e78ba904d654e1e21f930ba887233b921e12b2b52b5c3d39fa6cff8aa328985`)
shows PCIe0 reaching L0 at 3ms, WCN SWCTRL high, FSA ID09, DWC3 host
ready and a trained 1920x1080/HBR two-lane DP mirror. These are progress
markers; PCI enumeration subsequently fails with an out-of-resources
request, and this capture does not prove working Wi-Fi or PCIe1 NVMe.
PCIe1 still times out in receiver detection with its PHY ready and all
147 target programming readbacks matching.

Unplug repeatedly fails final release of USB UTMI and common PHY AUX
clocks. Both inherited gate requests remain enabled while their reset
source reports CLK_OFF. The native release path incorrectly waits for
active clock output while restoring a firmware-owned enable request.
ClockLib now verifies that the saved control bits were restored, allowing
native references to be released after reset. It retains strict cold
enable checks, voted-clock delays and failed native-owned halt retention.
The production ClockLib regression fails on the saved old implementation;
the repaired clock suite and the DWC3 role fixture using the real SM8450
clock graph pass.

The four GLink, PMIC/UCSI/PAN, QRTR/PDR and ChargerPD engines have moved
from the DXE directory to `Library/UcsiCrLib`. Their public headers are in
`Include/ucsi`, and `Include/Library/UcsiCrLib.h` exports the library API.
The core uses OSKAL status, types, memory, logging and interrupt APIs;
SMEM access and deferred-work wakeups are supplied by the caller.
`EFIUsbTypeCCrProtocol.h` aliases the shared connector state and enums,
preserving their UEFI ABI. `UCSICrDxe` now handles UEFI events, protocol
publication, PIL/SMEM adapters and supply lifetime.

The new reusable IPCC engine follows Linux's explicit receive-ID ACK and
CLEAR_ON_RECV_RD handling. Mainline and Android DTS agree on MPROC base
0x0ed18000, SPI229 (UEFI source261), LPASS client3/signal0. OSKAL registers
the IRQ and completes it. The ISR acknowledges a bounded ID batch, masks
only the LPASS doorbell while work is queued, and never parses GLink.
Deferred work services the signalled FIFO and re-enables that doorbell,
preserving notifications arriving during re-enable. An initial FIFO
snapshot and bounded continuation/deadline timers cover startup, partial
packets and outstanding commands. Idle UEFI service uses a one-second PIL
liveness watchdog without RX FIFO polling. The former 500ms runtime
GET_CONNECTOR_STATUS poll is removed: connector/topology notifications
and accepted role requests trigger refreshes, while command/ACK-only and
HPD-only notifications leave an idle PPM idle.

Native OSKAL now holds TPL_HIGH_LEVEL through its ISR callback and EOI,
then restores TPL. This follows Arm TimerDxe and prevents SignalEvent from
running lower-TPL work before IRQ completion; ArmGicDxe itself does not
raise TPL. Failed IRQ removal retains the driver and callback resources;
unload and ExitBootServices revoke the callback before releasing events
or the PMIC supply. Tests cover immediate registration IRQs, foreign
ownership, registration/removal failures, lost-wakeup races, burst limits
and retained cleanup, including 1000 idle watchdog calls without RX or
connector-status polling.

`UcsiCrLib.props` exposes the same five core sources to a WDF driver,
alongside its existing OSKAL provider imports. The OS adapter supplies
mapped MMIO, SMEM operations and an ISR-safe wake routine, and drains its
own deferred work before freeing the core. WDF uses the direct ISR handler
and serialized PnP/power callbacks; Stop requires PASSIVE_LEVEL. OSKAL
closes ISR access, drains running DPC/work-item callbacks with rundown
protection and detaches the owner synchronously. WDF owns D0 activation
and final interrupt-object deletion; OSKAL retains a stopped object for
rebind. For an IPCC restart, pass its retained interrupt config to Start.
The five cores compile without UEFI headers under `_KERNEL_MODE`, and the
same wire fixture plus WDF/IPCC concurrency tests pass using host NT/WDF
API-shape mocks. This is not a WDK or Windows hardware validation.

PCIe0's allocation request is 0x4100000 (65 MiB), alignment 0x1fffff,
against a 61 MiB window. Both Linux and Android specify that window;
enlarging it is unsupported. Native Type-0 configuration accepted all
32 Device slots on the immediate downstream link. UEFI scans all slots,
whereas Linux's `only_one_child()` restricts PCIe downstream/root-port
children to Device 0. Type-0 ignores the Device bits and can expose one
endpoint repeatedly. Thirty-two copies of a 2 MiB BAR plus the 1 MiB root
allocation match this capture, making aliasing the likely allocation
cause. PcieLib now rejects nonzero Device on that first downstream bus
before any MMIO. Functions of Device 0 and Type-1 BDFs behind another
bridge remain accessible. The aliasing regression fails on the saved old
library and passes after repair; board re-enumeration is still required
to confirm the capture's cause and actual endpoint BAR allocation.

The requested source repositories are now shallow, sparse reference
clones under `.vscode/`: `linux` at
`a53261b60834ce617cc4f3c7723dda8285c59789`,
`android_kernel_oneplus_sm8450` at
`449760504b3a75728eb60479f5b34dc51cb263ce`, and
`android_kernel_modules_and_devicetree_oneplus_sm8450` at
`877203a7cf9b79973d44a9db716490f95d820110`. Both OnePlus clones use
`oneplus/sm8450_v_15.0.0_oneplus_10_pro`. Driver and HDK DTS directories
are checked out, and PCIe scan/window behavior was rechecked there.
Clone identities are saved in `log18-reference-clones.json`.

Clock, native OSKAL, UCSI/QRTR/ChargerPD/IPCC, kernel-shape, USB/DP PHY,
DWC3, DP, PCI host/cold-init, native MU bus and CrDAL suites pass. PCIe1
receiver detection and the NVMe supply at the physical slot remain
unresolved. No board boot or flash was performed; the rebuilt image needs
a capture showing IRQ armed/receive-active markers, hotplug/unplug cleanup,
PCIe0 allocation and the PCIe1 diagnostics.

The exact virtualenv DEBUG `-s 0` build succeeded at 2026-10-07
02:46:56 +0800 (21 seconds in Stuart); all 133 images were verified.
All nine changed C units compiled with SECURE_BOOT=0, and 18 linked EFI
copies for the six affected modules match. Generated-FV native/storage
inclusion and APRIORI order, the UCSI HardwareInterrupt2 DEPEX, IRQ/clock
markers, and exact Android v3 BootShim+FD packaging pass. All 580 recorded
source and capture input hashes remained unchanged through the build.
Audits are in `.tmp/waipio-continuation/log18-build-inputs.json`,
`log18-artifacts.json` and `log18-manifest-fv-final.log`. Phase 22 artifacts:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1720320 bytes; SHA256 `496a7d802386364f2edb8cea8b300e9f84b121a0e5df93795d84d3c587049165`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `7966e264bd4f358ae995f3bc04446464dff0474a1c8d940e6a1c7b98b064f2c6`.

## Log19 receive IRQs, USB restart and physical buttons (Phases 23/24)

The complete refreshed `waipiolog.txt` (2026-10-07 03:05:36 +0800,
891999 bytes, 14297 lines, SHA256
`3669c34e47ed344ca6616d1ee184a80c035db88d00ab4204cd8d58aea727f8a4`)
contains both OSKAL IPCC armed and receive-active diagnostics. UCSI reaches
ready state and receives connector/PAN/HPD changes. PCIe0 reaches L0 in
3ms and the shell enumerates WCN6855 `17cb:1103`; the former 65MiB
allocation failure is absent. This confirms the receive path and PCIe0
enumeration in this capture, not a functioning Wi-Fi network driver.
PCIe1 still remains in receiver detection, with all 147 PHY readbacks
matching and its reset/refclk/GCC snapshots unchanged. Slot voltage,
translated PERST# and physical differential refclk remain unmeasured.

After the first USB role teardown, the capture reports 66 USB GDSC
re-enable timeouts (`0x6822f000`) and 57 combo PHY AUX startup timeouts
(`0x14905c = 0x80000001`). Native DWC3 left its wrapper BCR asserted at
Stop, and the next Start waited for the domain and clocks before releasing
that reset. The common USB/DP PHY similarly enabled AUX before releasing
its resets from the previous final release.
[Linux dwc3-qcom](https://github.com/sunflower2333/linux/blob/master/drivers/usb/dwc3/dwc3-qcom.c)
releases wrapper reset before preparing clocks, and
[Linux qmp-combo](https://github.com/sunflower2333/linux/blob/master/drivers/phy/qualcomm/phy-qcom-qmp-combo.c)
releases both GCC resets before enabling bulk clocks. DWC3 now performs a
complete reset pulse before domain/clock acquisition and completes the
Stop pulse rather than leaving its reset asserted. The common PHY now
releases its two resets before AUX/COM_AUX/PIPE acquisition. Failure
cleanup still retains resources when release fails. Independent reset
state models reproduce both restart failures on saved old adapters and
pass with the new ordering. Actual USB/DP hotplug recovery needs a fresh
board capture; no raw GDSC override was introduced.

HDK schematic sheet 56 maps Volume Up to PM8350 GPIO6, Volume Down to
PMK8350 RESIN_N and Power to PMK8350 KPDPWR_N. The
[mainline PMIC descriptions](https://github.com/sunflower2333/linux/blob/master/arch/arm64/boot/dts/qcom/pmk8350.dtsi)
and [Linux PON driver](https://github.com/sunflower2333/linux/blob/master/drivers/input/misc/pm8941-pwrkey.c)
identify the gen3 HLOS peripheral at SID0/0x1300, with pressed-high
RT_STS bits 7 (power) and 6 (RESIN). PM8350 GPIO6 is SID1/0x8d00,
electrically active-low. Android's GPIO key configuration uses VIN1,
input/pull-up and 15ms debounce; its RESIN key-code label disagrees with
the physical schematic and is not used for the HDK mapping.

`ButtonsCrLib` implements that mapping using the existing portable
SPMI/PMIC GPIO cores and OSKAL interrupts. `ButtonsCrDxe` adapts its queue
to SimpleTextIn and SimpleTextInEx: Volume Up emits SCAN_UP, Volume Down
SCAN_DOWN, and Power emits carriage return. It also supplies MU
ButtonServices boot-held volume state. The existing keypad device path
stays in the console list alongside the serial terminal. GPIO/PON edge
IRQs share SPMI's level-high PDC pin1, GIC source513. The driver preserves
the existing shared PDC setup instead of reinitializing all PDC pins.

Idle buttons perform no status reads and have no timer. An IRQ wakes
serialized work; one-shot timers cover debounce and held-volume repeat
(500ms initial, 100ms subsequent). Power never repeats. Boot/reset-held
keys must be released before generating input. Input and notification
queues are bounded; deferred callbacks cannot unload their executing
image. Native SPMI calls are serialized at HIGH TPL so IRQ delivery cannot
preempt a held arbiter spinlock. Stop detaches the OSKAL IRQ before
restoring PMIC/PDC state and closing events; failed cleanup keeps a
resident owner for retry. PON reset, pull and hardware debounce registers
are untouched. Only the selected GPIO and owned interrupt bits are
restored. `ButtonsCrLib.props` exposes the core for kernel consumers
alongside `spmi.props` and `pmic_gpio.props`; OS adapters supply deferred
wakeup and serialize their service/power paths.

ASan/UBSan integration compiles all nine real SPMI sources, PMIC GPIO,
button core and native input adapter against an independent arbiter/PMIC
and event model. It covers polarity, PMIC latch ACK, debounce, repeat,
1000 idle seconds without reads, both input protocols/notifications,
bounded queues, boot/reset-held suppression, read failure recovery,
unchanged PON controls, shared-bit restore, foreign ownership, failed
registration/removal and GPIO restoration, all six event creation
failures, unload retry and ExitBootServices. All eleven portable sources
also compile under `_KERNEL_MODE` using NT API-shape mocks without UEFI
headers; this is not a WDK or Windows hardware build.

Button, serial console, CrDAL, native OSKAL, UCSI/QRTR/ChargerPD/IPCC,
kernel-shape, clock, GPIO, PCI host/cold-init, DWC3, combo PHY and DP suites
pass. The source manifest now requires the compiled and packaged button
driver before BDS/console dispatch. The saved Phase 22 IRQ image remains
at `.tmp/waipio-continuation/buttons-before/qcom-hdk8450-irq.img`.

The exact virtualenv DEBUG `-s 0` build succeeded at 2026-10-07
03:28:20 +0800 (19 seconds in Stuart); all 134 images were verified.
All sixteen changed/new native C units compiled with SECURE_BOOT=0.
Generated FV/APRIORI order, ButtonsCrDxe HardwareInterrupt2 DEPEX and
mapping marker, 18 matching linked EFI copies across six affected
modules, and exact Android v3 compressed BootShim+FD packaging pass.
All 262 recorded source/capture input hashes stayed unchanged. Audits are
in `.tmp/waipio-continuation/log19-build-inputs.json`, `log19-artifacts.json`
and `log19-manifest-fv-final.log`. No board boot or flash was performed.
The next capture should show `ButtonsCr: PMIC IRQ source=513 ready`,
physical button navigation, and repeated USB/DP unplug/replug without
GDSC/AUX timeouts. PCIe1 is still unresolved. Current artifacts:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1728512 bytes; SHA256 `009631ebb069a442ae7f8e2f582ba874bc44c1bcbf3d9fd9e8dcb6720bec5c59`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `6006ff8e69c8f804f6f58bb687447337b1b849cbaf966fe1150c7625d5584c1c`.

## CRDK resource-core audit and build, 2026-10-07 05:05 +0800

The current `waipiolog.txt` still matches the saved Phase23 capture by SHA256
`3669c34e47ed344ca6616d1ee184a80c035db88d00ab4204cd8d58aea727f8a4`. It has
no hardware result for the latest reset/button or resource-core corrections.
PCIe0 WCN6855 enumeration and IPCC receive IRQ are present in that capture;
PCIe1 remains unresolved, and USB/DP restart needs a new boot of this image.

The shared GPIO core now guards unsupported interrupt pads and null/bounded
contexts. PDC initialization respects the declared range count and clears the
correct bit; type changes preserve non-type bits and validate both PDC and
shared SPI spans before writes. RPMh validates ACPI configuration and all TCS
register spans before IRQ publication. Native command DB validates all resource
tables and auxiliary data spans before publishing its protocol. Focused native
and NT sanitizer regressions cover these bounds and zero-write failures.

Windows CmdDB/GPIO/RPMh adapters use validated immutable configuration and
drained ownership. Real ARM64 WDK compile/link checks include actual property
sheet sources; they do not establish Windows installation or runtime behavior.
The full requested Windows PEP/DOD/PIL/UCSI/bus work and subsequent board ports
remain in progress.

The authorized `.venv` DEBUG `-s 0` build completed at 05:05:26 +0800 with
134 images verified. Generated FV/APRIORI checks pass, and the Android v3
compressed payload equals the actual 112-byte BootShim followed by the exact
FD. Evidence: `.tmp/waipio-continuation/crdk-build3.log`,
`crdk-manifest-fv3.log` and `crdk-artifacts3.json`. No board boot or flash
was performed. Current artifacts:

- `Build/WaipioCrPkg/qcom-hdk8450.img`: 1732608 bytes; SHA256 `b0c75688e03f3947ef02704901b9aa33e2eeacebcd331736d39af86a0a98e567`.
- `Build/WaipioCrPkg/DEBUG_CLANGPDB/FV/SM8450_EFI.fd`: 4194304 bytes; SHA256 `f9098676f35529b31ed1b67926de676936eeb9c1a17a5bfb57629eb555628bea`.

## October 7 shared clock target continuation

The capture still hashes to `3669c34e47ed344ca6616d1ee184a80c035db88d00ab4204cd8d58aea727f8a4`.
The clock target graph now lives in a portable Crane library; native wrappers
retain the resource ABI, parent edges are immutable descriptions, and reset
updates use the context's mapped GCC base with register bounds and preserved
non-reset bits. Shared GPIO/RPMh/command-DB fixes remain in this firmware.

The requested virtualenv `./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` completed
at05:37:05 +0800 with134 images verified. Generated FV/APRIORI/storage chain and
Androidv3 gzip payload equality with112-byte BootShim+FD pass. Image1732608 bytes:
`4d459d2eb62db14196808292a4e54e4df4627772408bdecff8793bfe6126c81a`;
FD4194304 bytes: `d197528934f9ea6093ccb483c8684da508eec75973e76fe05c2717c073a2b5e7`.
Logs/manifests are `.tmp/waipio-continuation/crdk-build5.log`,
`crdk-manifest-fv5.log` and `crdk-artifacts5.json`.

The first target extraction rebuild exposed missing ClockLib/SynchronizationLib
INF dependencies, now corrected. It also exposed `build_uefi.py` swallowing
subprocess failures. The wrapper now stops and propagates exit/signal status
before CI copying or later stages; real subprocess regressions prove failed
builds leave stale artifacts untouched. Firmware/package validation remains
separate from hardware behavior. No fresh board boot or Windows execution was
performed; PCIe1 link/power and USB/DP behavior still require new evidence.

## Shared clock source/divider build, 2026-10-07 05:58 +0800

The virtualenv qcom-hdk8450 DEBUG -s0 build succeeds with134 verified images.
Shared ClockLib now owns fractional external source/divider programming, complete
mapped register bounds and failed relatch recovery. Native wrappers share those
operations with ClockCr revision2; Linux byte-interface parent ordering is fixed.
Clock, native/NT ownership, mapped kernel resource, DWC3 and actual Windows clock
adapter sanitizer suites pass. All36 core/Common units and CmdDB5/RPMh9/GPIO13/
Clock12 actual imported units compile/link against real ARM64 WDK. Generated FV
and exact Androidv3 gzip BootShim112+FD4194304 payload equality pass.

Image1732608 SHA256 `a4d37f47c5e06989770e44155c64d269b2cb91ffd6cd9039962e3fc133073026`;
FD SHA256 `d6b327e4c376281fdd3536b654c4b9402a7685035c7d594fdf22b2567032a114`.
Capture remains log19 SHA2563669c34e; no boot validates USB/DP or PCIe1 behavior.
Windows compile/link is unsigned development evidence, not runtime/signing.

## Shared SPMI/interconnect ownership build, 2026-10-07 06:23 +0800

SPMI v7/v8 now preflights complete mappings, APID tables, channel transfers and
IRQ ranges, rejects invalid trigger types before writes, and drains OSKAL before
retiring callback storage. Failed detach retains ownership for retry. Buttons
uses the shared interrupt stop/resume helper. Interconnect now restores uncertain
hardware completions, retains failed rollback/init cleanup and preflights full
VCD capacity. Its native adapter retains callback code on failed init/install
cleanup and refuses unload while path handles remain in use. Successful display
handoff retains its existing votes at ExitBootServices.

Native/NT ASan/UBSan SPMI and ICB suites, native button and actual native ICB
adapter regressions pass. SDK export contains20libraries/119inputs/36portable C
units; all37core/Common units compile/link with real ARM64 WDK headers/libraries.
The requested virtualenv DEBUG -s0 build succeeds with134verified images.
Generated FV/APRIORI and exact gzip BootShim112+FD4194304 equality pass.

Image1732608 SHA256 `22cd621aa84998827bfc9e736f104f0068b58bd709f3563b1b51bccf5b640cfe`;
FD SHA256 `a73bd9e200cb9405fd034ca9fe7aaf46f24671f1b830caa486a855b27eeff0a1`.
Evidence: `.tmp/waipio-continuation/crdk-artifacts8.json`, `crdk-build8.log` and
`crdk-manifest-fv8.log`. No boot/flash or Windows execution was performed.
The current capture remains log19 SHA2563669c34e; PCIe1 and USB/DP behavior
require refreshed hardware evidence. Remaining Windows providers, permanent PEP
recovery/bus, PIL/UCSI/DOD and board ports continue in the active plan.

## PDC bounds and combined Windows resource provider, 2026-10-07 06:35 +0800

PDC now preflights complete mapped spans, type/enable words and shared SPI
configuration, rejects overlapping pin/GIC intervals and invalid enable values
before MMIO. Native/NT sanitizer tests include non-TLMM PDC1/GSI513; the saved
pre-fix source fails the first address-overflow vector. Kernel resource and GPIO
register suites pass. Windows RPMh now owns a private interconnect voter and
publishes its query ABI, preserves handles across D0 sleep and retains IRQ/MMIO
ownership if clearing votes fails. Actual adapter/shared-voter tests, all37
core/Common WDK units, RPMh12/GPIO13 imported project units and actual ICFG AML
evaluation pass. Permanent-removal journal and system-sleep policy remain work
in progress. Windows ACPI is a separate development artifact.

The requested virtualenv DEBUG -s0 build succeeds with134 verified images.
Image1732608 SHA256 `35d7df5b8680538f4c9b44c72c4e658e7a9c74111c04d9f78002a8a14507b047`;
FD4194304 SHA256 `b7e586c5ffdfd5c89b964e588a3de0bbb33567907d9500d96b6d4249b14f1d5f`.
Generated FV/APRIORI and exact Androidv3 gzip BootShim112+FD equality pass.
Evidence: `.tmp/waipio-continuation/crdk-artifacts9.json`, `crdk-build9.log`,
`crdk-manifest-fv9.log`; Windows `Build/resource-wdk-check9.log` and
`Build/icb-acpi-check9.log`. Capture still log19 SHA2563669c34e; no new board
boot confirms PCIe1 or USB/DP behavior.

### Native IRQ routing and Windows SPMI follow-up (build10)

Android's qcom-pdc spi_configure_type shows that an edge route must clear the
shared SPI configuration bit. The new before-source test reproduces the stale
level bit; native/NT fixed vectors pass. Native GPIO/PDC convenience helpers
now claim the exact OSKAL owner before route writes, retain failed cleanup,
restore inherited field bits after synchronous retirement, reject duplicate or
active reinitialization, and scan GPIO table holes/other contexts correctly.
Native OSKAL table mutations run at HIGH priority and reject entry from a live
ISR. Actual native OSKAL/core failure-injection tests pass; saved PDC and GPIO
helpers respectively remove a foreign owner and fail null-context UBSan.

Windows PIO0 includes direct PDC1/GSI513 as virtual384 with GDPI whitelist.
SpmiCr uses five Linux register windows and the shared OSKAL WDF DPC route,
with callback/request rundown and retained failed cleanup. Actual adapter
sanitizers and GPIO13/SPMI17/all37 core/Common ARM64 WDK checks pass. Full Windows
DSDT compiles0errors/28warnings/13remarks,16333bytes; actual ACPICA interpretation
checks GpioInt, seven GPIO GSIs, GDPI and SPMI GCFG against Linux. This ACPI
artifact is separate from the native firmware image. Permanent PEP/bus recovery
and complete sleep policy remain pending.

Requested virtualenv DEBUG -s0 native build10 succeeds07:17:54 +0800; generated
FV/APRIORI/storage checks and Androidv3 exact gzip BootShim112+FD equality pass.
Image1732608 SHA256 `d1766028b9b1e65a8fb891ab579ec65044bb839d047ebaa732e8c09f2ad55dd8`;
FD4194304 SHA256 `1449f483a42982997ccd2a3e158e0c6ed641980d4cd5ac0191a6c27e7dba19ee`.
Evidence: `.tmp/waipio-continuation/crdk-artifacts10.json`, `crdk-build10.log`,
`crdk-manifest-fv10.log`, `gpio-pdc-irq10.log`, `pdc-edge-before10.log`,
`pdc-owner-before10.log`, `gpio-owner-before10.log`; Windows
`Build/spmi-adapter10.log`, `Build/gpio-spmi-resource-wdk12.log`,
`Build/resource-acpi10/manifest.json`. Same log19 SHA3669c34e remains current;
no hardware boot validates USB/DP or PCIe1 behavior with this image.

## CRDK SMEM ownership checkpoint11 (2026-10-07)

The shared remote-mutex core now preserves failed release ownership and checks
configuration/IRQL before MMIO. The native SMEM adapter retries retained release
before a new allocation, preserving foreign ownership and legacy duplicate
semantics. Native/NT mutex, SMEM parser, actual DXE and SEC sanitizer tests pass.
Windows SmemCr uses explicit pointer leases, normal write-combined shared
memory and the separate noncached lock3 register. Actual adapter+core lifetime
checks and all9 project/all38 SDK+Common ARM64 WDK units pass. Actual Windows
DSDT compiles0errors/28warnings/13remarks,16430bytes, and ACPICA verifies its
SMEM resource order/GCFG against mainline DTS. That AML is separate from UEFI.

Requested virtualenv DEBUG -s0 build11 succeeds07:32:46.561645 +0800,134images
verified. GeneratedFV/APRIORI/storage and Androidv3 exact BootShim112+FD gzip
equality pass. Image1736704 SHA256
`33ff51d961ce1b5452e6994af7ae6712fbda4ef0a941d25237ac4ce0bdce5852`;
FD4194304 SHA256
`577e9885f882a2e29e5e1bb77f6d238a8d74f25a8cebbfc7b1f2e39a10256475`.
Evidence: `.tmp/waipio-continuation/crdk-artifacts11.json`, `crdk-build11.log`,
`crdk-manifest-fv11.log`, `smem-mutex11.log`, `smem-dxe11.log`, `smem-core11.log`,
`smem-sec11.log`; Windows `Build/smem-adapter11.log`, `smem-resource-wdk11.log`,
`crdk-wdk11.log`, `resource-acpi11/manifest.json`. Capture remains log19
SHA3669c34e. No board boot proves USB/DP or PCIe1 with this image.

## Shared clock cleanup checkpoint12 (2026-10-07)

The branch core now retains an acquired parent prefix and separate gate ownership. A partial disable retries only unreleased dependencies, preserving a foreign owner on a previously released parent. Failed cold-enable undo retains its cleanup reference, including a failed parent acquisition that retained one; re-enable is rejected during incomplete release. Saved-source regression reproduces the lost foreign reference. Native/NT ownership, generic clock/target/DXE, USB/DP PHY, PCIe cold, CrDAL and actual Windows clock adapter checks pass. Linux and Android `drivers/clk/clk.c` first/last reference accounting informed the dependency order; their void disable callback does not implement our failure-bearing retry contract.

Managed SDK export13b is synchronized; all42 core/Common and13 actual ClockCr ARM64 WDK units compile/link. Requested virtualenv DEBUG -s0 build12 succeeds08:43:40 +0800,16sec. GeneratedFV/APRIORI/storage and exact Androidv3 gzip BootShim112+FD equality pass.
Image1736704 SHA256 `9ea88947b3bbb1ad244ba8cf43bef7da7d89357f1bf99bba14d1809fe7425ddd`.
FD4194304 SHA256 `d8c5c3958766e1790a444ca89689cab2c9db21ccbe226a4195b0167ee6aac6d0`.
Evidence: `.tmp/waipio-continuation/crdk-artifacts12.json`, `crdk-build12.log`, `crdk-manifest-fv12.log`, `clock-parent-before13.log`, `clock-parent13-final.log`, `clock13-clock.log`; Windows `Build/crdk-wdk13-clock.log`, `resource-wdk13-clock.log`. Capture remains log19 SHA3669c34e. No board boot confirms USB/DP or PCIe1.

## Shared regulator and reserved PEP checkpoint16 (2026-10-07)

The native RPMh adapter now uses the shared RpmhRailLib supply graph for fixed contracts, shared parents and uncertain acquire/release recovery. Native actual-DXE and native/NT graph tests pass. Shared owner banks and PEP journals reserve all future owner keys before permanent publication, retain capacity across Close and preserve foreign leases during rollback. Kodiak's display PMIC driver/vote qualifiers are retained and explicitly matched. These owner-bank/journal APIs are foundations for Windows; native UEFI does not publish Windows PEP. A saved regulator-bank regression reproduces absent-owner rollback waiting on a foreign failed acquire; the fix leaves that foreign cleanup untouched while readiness stays gated.

Managed export17b synchronizes20libraries/139inputs. Native/NT clock/rail/ICB and actual Windows clock/RPMh/DPM/permanent-store sanitizers pass; all51 SDK/Common and92 actual resource-project ARM64 WDK17 units compile/link. Windows rail/NPA binding, permanent provider broker/removal/readiness and consumer integration remain unfinished. This is no Windows install/signing/runtime result.

Requested virtualenv DEBUG -s0 build16 succeeds09:56:33, Oct.07 2026 +0800. GeneratedFV/APRIORI/storage and exact Androidv3 gzip BootShim112+FD4194304 equality pass. Image1736704 SHA256 `7bbe5ff0d09259e893b2d1daefce541ccfb291b486572002fdb51f2a2e69f422`; FD4194304 SHA256 `ab2de19b5d584a4220904a91e9b8d33be7cc1e7b581a4bd5d4547a2dc2fcb5d6`. Evidence: `.tmp/waipio-continuation/crdk-artifacts16.json`, `crdk-build16.log`, `crdk-manifest-fv16.log`, `clock-reserved17c.log`, `rail-foreign17-before.log`, `rail-foreign17-after.log`, `icb-reserved17.log`; Windows `Build/clock-reserved17c.log`, `rpmh-reserved17.log`, `pep-reserved17.log`, `pep-dpm17.log`, `crdk-wdk17.log`, `resource-wdk17.log`. Capture remains log19 SHA3669c34e; no board boot proves USB/DP or PCIe1 with this image.


## Regulator, PIL recovery and PCIe diagnostic checkpoint 19 (2026-10-07)

`waipiolog.txt` remains the same log19 capture (SHA256 `3669c34e47ed344ca6616d1ee184a80c035db88d00ab4204cd8d58aea727f8a4`, 891999 bytes). It shows active UCSI IRQ 261, PCIe0/WCN enumeration and PCIe1 Detect.Quiet. Earlier USB/DP reset-order fixes remain included; this capture predates the current image and does not test their recovery.

Raw regulator helpers now reject offset bases (+4/+8/+12), wrapped field spans and out-of-field voltage/mode values before TCS writes. Shared supply graphs reject malformed or overlapping tuples. Saved failing runs reproduce the old address-wrap and offset-base acceptance. The wire limits follow Linux's [RPMh regulator fields](https://raw.githubusercontent.com/sunflower2333/linux/master/drivers/regulator/qcom-rpmh-regulator.c); [cmd-db resource matching](https://raw.githubusercontent.com/sunflower2333/linux/master/drivers/soc/qcom/cmd-db.c) confirms that low 4 bits select commands inside a VRM slot. Native/NT graph, owner-bank and actual native DXE tests pass. These malformed-input defects are not established causes of PCIe1 Detect in this capture.

PIL now retains interrupted secure calls beyond the bounded resume attempt, including the original arguments and changing 64-bit continuation cookie. Stop retires that exact operation before PAS shutdown or prerequisite release. Native startup/unload/metadata cleanup respects the pending owner; the native secure bridge clears x8..x17. A successfully resumed shutdown is recognised, avoiding a duplicate command that firmware may reject as already stopped. Native bridge+actual DXE, native/NT core+qualified helper and an independent eight-case shutdown review pass. Failure still retains metadata, votes and crypto ownership for retry.

PCIe1 timeout now reports exact supply identities, requested and cached mV/mode, adapter references, cached acquisition readiness, live ownership masks, PERST assertion/release and training timestamps. A bounded read-only PHY snapshot uses all 43 Android PCIe1 debug offsets after checking the complete region. PCIe cold and host sanitizer suites cover the bounds, powered read timing, cache preservation and optional timer fallback. No reset delay, link speed or equalization was changed. Cached supply readiness is not a voltage measurement; the default M.2 supply is the external DC-fed buck path shown by the schematic. Physical PCIe1 recovery remains unresolved.

Windows now has a private 14-rail HDK owner service and explicit PEP rail profiles, plus PIL/UcmUcsiCx/DOD consumers. UCSI retains complete notification snapshots and uses OSKAL IRQ acknowledgment followed by DPC/passive work. Fixed a UART/display hardware-ID collision: MDCR uses ALCR000E, UART retains ALCR0009. Managed SDK 23 libraries/160 inputs synchronize; all 59 SDK/Common C units plus one clang ARM64 assembly unit and 94 resource-project C units compile/link against real WDK. PIL 14 C + 1 ASM, UCSI 19 C and DOD 6 C checks also pass. Full Windows DSDT/ACPICA checks pass (16925 bytes, 0 errors, 28 warnings, 13 remarks) with default-off PIL/UCSI/MDSS gates and matching INF IDs. This separate Windows AML is not the native firmware table. Permanent broker/NPA/removal/readiness, qualified PAS/display platform backends, complete bus/PEP integration and the four new board ports remain unfinished; no Windows installation or hardware execution is claimed.

Requested virtualenv DEBUG -s0 build 19 succeeds at 10:41:18, Oct.07 2026 +0800 with 134 verified images. Generated FV/APRIORI/storage and Android v3 exact gzip 112-byte BootShim + 4194304-byte FD equality pass. Image 1736704 bytes SHA256 `d4d7ab7b77aabb44b4e62cce2d79b40b083f513130cb66aa19e802e94bede7f1`; FD 4194304 bytes SHA256 `670a6a6745bf3307f081e56f02e41b94a5457609a7666397fbab464c93a0e563`.

Evidence: `.tmp/waipio-continuation/crdk-artifacts19.json`, `crdk-build19.log`, `crdk-manifest-fv19.log`, `rpmh-base-before19.log`, `rpmh-base19.log`, `rpmh-rail19.log`, `log19-pcie-diagnostic-cold-agent4.log`, `log19-pcie-diagnostic-host-agent4.log`, `pil-port/native-pil6.log`, `pil-port/shared-pil6.log`, `pil-independent-review/fixed.log`; Windows `Build/crdk-wdk19/artifacts.json`, `resource-wdk19/artifacts.json`, `pil-wdk3/artifacts.json`, `ucsi-wdk-validated2/artifacts.json`, `mdss-wdk-agent4/artifacts.json`, `resource-acpi19/manifest.json`.

## Checkpoint20: retained clock cleanup and foreign-owner protection

The supplied `waipiolog.txt` remains byte-for-byte capture19 (SHA2563669c34e47ed344ca6616d1ee184a80c035db88d00ab4204cd8d58aea727f8a4). It shows active IPCC IRQ261 and PMIC/UCSI/PAN connector/HPD notifications, PCIe0 WCN6855 enumeration, repeated USB GDSC/AUX timeouts and PCIe1 DetectQuiet. No new image boot or endpoint-supply measurement is established by this capture.

Saved-source actual frontend/core fixtures demonstrate three additional defects: retained native failed acquisition could be treated as an uncertain observer and never actively released; eager rate programming could change a foreign RCG before denying acquisition; automatic pipe preselection could change a foreign mux. Exact acquisition ownership, reference-aware inherited GDSC restoration, rate programming inside core acquisition and atomic unowned source+acquire fix those paths. USB, DP, MDSS, UFS and PCIe preserve returned cleanup ownership. GPI and I2C now keep exact acquired masks, retry failed unwind before reacquisition and avoid duplicate healthy GPI prefix votes. Live or uncertain GPI DMA remains pinned.

Existing focused native/core sanitizer tests, new actual ClockCr driver/core fault tests and GENI/native bus tests pass. The portable catalogue now exports26 libraries/173inputs; real ARM64 WDK SDK/Common67C+1clangASM and six resource projects96C compile/link. ARC/NPA and qualified Common PIL/MDSS backends have separate actual-source/WDK receipts, while their missing permanent host/native handoff and runtime qualification gates remain disabled.

Authorized `source .venv/bin/activate` and `./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` succeeds at 2026-10-07 11:17:58.034876 Asia/Taipei. Generated FV/APRIORI/native storage manifests pass; Android boot v3 gzip payload equals BootShim112bytes + FD4194304bytes exactly. Image `Build/WaipioCrPkg/qcom-hdk8450.img` SHA256 24c8883c729aeb70a0d23508525cc9f68154d594b1e212d26c42c15fef7724e0. Receipt `.tmp/waipio-continuation/crdk-artifacts20.json` records source and artifact hashes. Hardware USB/DP recovery and PCIe1 power/link remain unverified.


## October 7 capture35: SD supplies and DP console margins

The new capture enumerates a full-speed keyboard (258a:0143), obtains six
interrupt-IN reports, then records xHCI transaction error4. The user confirms
it is attached to the DP hub. The capture contains no intervening DWC3 role or
PHY/clock change at the first failure. Bounded endpoint recovery terminates;
the cause of the physical transactions still needs a device test after the
current source fixes.

DP now visibly renders shell frames on its own 1920x1080 GOP framebuffer.
It does not periodically copy the DSI surface. DEBUG startup bars remained
outside GraphicsConsole's centered text rectangle; DpGop now starts with a
full black framebuffer before publication, including DEBUG builds. Normal
partial BLTs and the retained surface on reconnect preserve their semantics.
The late-console connection check also follows the actual ConSplitter
BY_DRIVER/BY_CHILD relationship: Shell ConsoleLogger temporarily replaces the
system-table console handle while forwarding to the original console.

The appended SD initialization errors exposed a native inventory omission:
SDCC2 requests `ldoc9` and `ldoc6`, but the native RPMh provider did not register
them, even though the portable rail graph already did. The actual native
inventory now includes both rails and their shared `bobc1` parent at 3296mV/AUTO.
SD outputs remain 3008mV/HPM, matching the board supply ranges in
[the HDK DTS](https://github.com/sunflower2333/linux/blob/master/arch/arm64/boot/dts/qcom/sm8450-hdk.dts).
The actual RPMh driver regression uses the same board data header and fails
against the previous inventory; it checks supply ordering and shared-parent
retirement. Initialization failures now log the specific stage and pending
power causes at ERROR level. The older standalone TIMEOUT in the capture is
not yet independently diagnosed; these details will locate any remaining
failure in the next device run.

Actual-source RPMh, native SD resource/IRQ/GPIO, native PIO host, combined
MMC/card protocol, and DP debug/release lifecycle/transport/pixel tests pass.
No successful physical SD-card access is claimed by those tests.

The xHCI transfer and command rings now publish the Cycle ownership bit in a
final volatile control-word store after a memory fence. The old field writes
could make ownership visible before the descriptor body on ARM, even if the
later doorbell had its own barrier. Commands first copy an explicitly unowned
TRB for both producer-cycle phases; LINK publication uses the same helper.
This follows the per-TRB ordering in Linux queue_trb. The change does not add
Linux's separate whole-TD first-TRB gate. First transaction failures also
record the qualified live descriptor words and mapping identity.

The actual-source weak-DMA-visibility regression fails against the previous
code and passes for both cycle phases, command copies, ring wrap and
control/bulk/interrupt descriptors. Existing endpoint recovery, event-ring and
USB bus sanitizer suites pass, and the whole scheduler compiles with the real
ARM64 headers. This fixes a demonstrated publication-order defect; it does
not yet establish the cause of the physical keyboard's transaction errors.

The requested virtualenv DEBUG -s0 build35 succeeded at 19:22:36, October 7,
2026 (Asia/Taipei). All 138 PE images, generated FV/APRIORI/storage manifests,
956 unchanged source inputs and the Android v3 gzip payload equality to
BootShim112 + FD4194304 were verified. The image is
`Build/WaipioCrPkg/qcom-hdk8450.img` (1949696 bytes), SHA256
`71f5a079e496b31852a4a23ab167d538d104debcc002a808122ad6553be33524`.
Evidence is in `.tmp/waipio-continuation/crdk-artifacts35.json`,
`crdk-build35.log`, `crdk-manifest-fv35.log`, `xhci-publication35-root/`,
`xhci-endpoint35-final/`, `xhci-event35-final/`, `usb-bus35/`,
`dp35-root.log`, `sd35-rails.log`, `sd35-resources/` and `sd35-mmc/`.
Image34 was preserved as `qcom-hdk8450-build34.img` in the same receipt folder.
USB keyboard reconnect, DP margins, SD hot insertion/access and ESP-variable
reboot persistence still require a device test. PCIe1 remains unresolved.

## October 7 capture36: halted USB teardown and SD power handshakes

The user reports the DP glitch fixed. This capture shows successful DP console
attachment and no repeated USB transaction-error4 episodes. Its USB device is
3554:fa09, however, so it does not establish recovery of the previous
258a:0143 keyboard. The saved capture is `capture36.log`, SHA256
`e5c7c23b2ec1c498d487e318d2b94af766cf04ada196f4bd7e0e3ce2ac4db3ec`.

USB teardown still attempted DisableSlot after DWC3 had already halted the
controller. Whole-controller Stop now frees slot allocations only after a
confirmed halt and successful protocol removal, without issuing commands to
the halted controller. Failed halt or protocol removal retains ownership.
Live individual-slot removal still requires successful DisableSlot completion.
Shared cleanup also frees the outer configuration-descriptor array and all
endpoint ring allocations. The actual-source regression fails against the
old halted-Stop path and passes with address/undefined-behavior sanitizers for
both context sizes, including failed-stop retention.

SD now resolves and votes both supplies, but the capture fails first at the
400kHz clock stage and then repeatedly at reset with a pending power request.
The native resource adapter now services and acknowledges power requests
while the portable core waits for IO_HIGH or powered RESET_ALL/BUS_OFF. This
follows the write-side power synchronization in
[Linux sdhci-msm](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-msm.c)
and the supplied OnePlus Android kernel. A previous acknowledgment is accepted
only with independently known matching state; errors retain reset ownership.
The worker and synchronous service share the same resource ownership rules.

Clock enable writes only writable enable bits and polls the resulting state.
Failure diagnostics now include clock, power, reset, host-control, DLL and
power-IRQ registers. The first physical clock Device Error is not yet fully
explained; those readbacks distinguish any remaining cause. ExitBootServices
uses command/data reset after startup has proved inherited DMA stopped and
subsequent operation is PIO, avoiding a regulator transaction at callback
priority that cannot service it. Without that startup proof, full-reset
failure retains resources.

Actual SD resource/OSKAL/GPIO, native host, full MMC/PIO-card protocol, portable
native/NT cold-init, USB endpoint/event/publication/bus and ARM64 compile
checks pass. The requested virtualenv DEBUG -s0 build36 succeeded at 19:46:00,
October 7, 2026 (Asia/Taipei). Generated FV manifests, 956 unchanged source
inputs and exact Android-v3 BootShim-plus-FD payload equality pass. The image
`Build/WaipioCrPkg/qcom-hdk8450.img` is 1953792 bytes, SHA256
`db071d449ceb40d9d0acf284eb2b4cc81897e918386e304e8dc3a47fc49b3539`.
Receipt: `.tmp/waipio-continuation/crdk-artifacts36.json`. Image35 is retained
as `qcom-hdk8450-build35.img` in that folder. USB reconnect and SD insertion,
enumeration and access need another device test. PCIe1 and physical ESP-variable
reboot persistence remain unverified.


## Capture37: SD identification and USB reconnect evidence

The refreshed capture reaches SDHC OCR/CID/RCA/CSD twice. Each pass contains one
recovered timeout between CMD8 and the final OCR response. The old generic error
cannot distinguish CMD55 from ACMD41 or a hardware interrupt timeout from the
software wait limit. CMD5 never reaches hardware on this SD-only host. The next
image records command, argument, application-command context, failure stage,
latched errors and six qualified register reads before reset clears the evidence.
It also reports successful MMC media initialization and removal at DEBUG_INIT.
No SD clock, supply or retry timing adjustment is justified by this capture.

The keyboard completed17 interrupt-IN transfers before the final error burst.
Both interfaces report two transaction errors each, then stop and re-enumerate
successfully as3554:FA09. The previous halted-controller DisableSlot failure is
absent. This establishes class rebind, not keystrokes after rebind; the latter
still needs a device test. DP link/frame progress remains healthy. PCIe1 still
stays in Detect.Quiet, while PCIe0WiFi links normally.

The ACPI image exit status1 is EFI_REQUEST_UNLOAD_IMAGE: this one-shot installer
requests unloading after publishing its tables. DXE now labels that exact status
as a requested unload. The installer also rejects an empty RAW inventory and
preserves unexpected ReadSection errors instead of misreporting completion.
No ACPI or IORT capability was enabled by this correction.

For the next capture, reconnect the same keyboard and verify actual keystrokes;
insert SD, run `map -r`, and try listing the card filesystem. Keep the new
`SdhciCr:` request lines and `MMC card ready:` message in the capture. An SD card
without a UEFI-supported filesystem can expose block storage without an FS alias.

Build37: `Build/WaipioCrPkg/qcom-hdk8450.img`, SHA256 `73ba8cce45826d38405afe87696c1a2e4e8c70adb34de7d921365a06c9538327`. Requested virtualenv DEBUG `-s 0` build,957source hashes, FV inventory and Android payload verification pass. Host sanitizer tests pass; physical card I/O and keyboard reconnect remain to be tested.


## Build38: allow SD partition reads during media reconnection

`CheckCardsCallback` held the MMC I/O busy flag across protocol reinstallation.
The DXE core reconnects storage drivers synchronously during that call, so their
partition-sector reads failed with EFI_NOT_READY. The driver now releases I/O
exclusion before reinstalling. The scan guard still prevents recursive scans
and instance destruction. A failed reinstall remains pending, and a new media
change discovered by nested I/O is preserved.

The actual MMC/native-host/PIO fixture reproduces the blocked read with the old
source. The fixed fixture passes nested reads, removal, reentrant-stop refusal,
reinstall retry and nested-reset notification retention; the full MMC class
fault suite also passes. Physical SD filesystem access still needs a test.

The user clarified that capture37 used only the working wireless receiver;
it contains no current failure episode for the wired keyboard. Capture the
wired keyboard alone through plug/unplug/replug on this image.

Build38 image SHA256 `972cfd135e0439864d0cd805576693f041b794e3ec35a778c834b55ce2786262`.
Requested virtualenv DEBUG `-s 0` build,957source hashes, FV inventory and Android
payload verification pass. Build37 SD diagnostics and ACPI corrections remain.

The user confirms this same SD card exposes an FS alias when present before boot, but not after hotplug. That supports the reconnect-path diagnosis; retest hotplug specifically on build38. Independent review and a second actual MMC/SDHCI/core sanitizer run pass.

## Native checkpoint 39

The refreshed capture shows SD hotplug mounting FAT as FS2 after initialization,
and removal clearing its alias. An initial ACMD6 timeout still precedes the
successful retry. Wired keyboard 258A:0143 now has a captured failure after four
and two successful input transfers; the wireless receiver works until removal.

This image adds the [SMEM RAM partition producer](RamPartitionCrLib.md), including
DDR above the fixed initial map while preserving all existing reservations. It
also adds Android's eight-microsecond CRC-engine turnaround after a response
without CRC, extra SD enable/mask diagnostics, and HID SET_IDLE before keyboard
polling. These source corrections do not prove the remaining hardware USB/SD
failures are resolved. Shared MDSS PHY teardown now requires bounded control
readbacks before releasing its dependencies.

The requested DEBUG `-s 0` build passed at 21:30:15 on 2026-10-07 (+0800).
Image SHA256: `e363d0846ca9f60107e56db2ab4235140505d5c237ed28428d198ecf2defaa02`.
The receipt is `.tmp/waipio-continuation/crdk-artifacts39.json`: 970 frozen inputs,
139 EFI images, generated FV/provider order, and exact Android boot payload
verified. Root and independent RAM parser/DXE sanitizer suites pass. Board
validation needs the new RAM discovery log and `memmap`, SD insertion/removal,
and the wired keyboard reconnect. Multicore and extended MDSS modes remain in
separate development and are not included in this checkpoint.

## Native checkpoint 40

Capture39 (`waipiolog.txt`, SHA2568fa585d61513289914f5a9704dd4fe6b1d98c527fd6afd96799dcd007e9e5607) retrieves SMEM402 but the original domain filter finds no banks; shell reports534MiB. Qualcomm's default domain0 is now accepted for system SDRAM, with explicit modem-only/unknown domains still excluded. The actual board domain awaits the next log. Failed-table metadata diagnostics are bounded to the private snapshot.

SDHCI normal interrupt enable bit15 is fixed zero. Error-summary handling now uses individual error enables, fixing the observed hardware8000/0001 versus softwarezero error mismatch. Physical removal terminates the MMC OCR loop immediately. The earlier ACMD6/ACMD41 timeouts with no raw IRQ remain under investigation. USB LS/FS periodic endpoint contexts now use packet size for AverageTRBLength; successful SET_IDLE is logged once. Neither is claimed to resolve the wired keyboard's hardware transaction errors until tested.

Exact virtualenv `./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` passed at21:51:42Oct07 2026Taipei. Image1957888bytes SHA2564767a75b139ffa8ffb504a7f429829e02400db9ce81290335a852555e494aa3b; receipt `.tmp/waipio-continuation/crdk-artifacts40.json`.970inputs,139EFIimages,FV/APRIORI andAndroidBootShim/FDlayout verified. Relevant host sanitizer suites pass, including old-source failing regressions. Hardware tests requested: memmap, SDhotplug/removal, wiredkeyboardreconnect. MP and extendedMDSS remain staged.

## Native checkpoint 41

Capture40 now confirms five SMEM system banks (default domain0), 12GiB physical
DDR and0x270500000 additional usable bytes. The accompanying SMMU allocation
`Unsupported` was caused by publishing these new GCD regions with WB capability
only. They now advertise supported WB/WC mappings while retaining WB/XP as
the initial mapping. Actual RAM publication and SMMU allocation tests cover
low/high physical regions, the WC/XP transition and WB/XP restoration; the old
producer fails this regression.

SD remains a hardware validation item: ACMD41 supplies an OCR but the capture
has no completion IRQ, followed by command timeout errors. This checkpoint
uses the atomic32-bit status read/selected acknowledgement in the supplied
[Android SDHCI driver](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/mmc/host/sdhci.c#L3446-L3464).
It clears only observed stale events before a command, preserving an unrelated
cause that arrives after the snapshot. The earlier slow-clear40us workaround
is for old Qualcomm host revisions and is absent in this SM8450 Android driver;
it is not enabled speculatively.

The [Android OCR loop](https://github.com/OnePlusOSS/android_kernel_oneplus_sm8450/blob/449760504b3a75728eb60479f5b34dc51cb263ce/drivers/mmc/core/sd_ops.c#L111-L150)
exits on a command error. MmcDxe now similarly distinguishes a failed transaction
from a valid busy OCR, preserving the existing bounded full-initialization retry
policy. A known SD card is no longer misclassified as MMC after a CMD55 error.
The first16 completed commands per initialization and first/seen/last raw IRQ
poll snapshots provide bounded evidence if completion still fails. None of
these tests establishes that the board-level missing completion is resolved.

Actual-source native/NT core, native host debug/release, full MMC/native/PIO
integration, MMC class and Windows core/SDPORT sanitizer suites pass. The new
late-interrupt and failed-OCR regressions fail against the saved previous source.
Build and physical test results follow in the checkpoint receipt. MP and extended
MDSS hardware modes are still staged outside this image.

Build41 completed at22:20:04 on2026-10-07 (+0800). Image SHA256
`d2b524b4ad353ad2bb8a3ef3a9b2032c8c046c22426b0a3ba457cf114e591218`.
Receipt `.tmp/waipio-continuation/crdk-artifacts41.json` verifies970unchanged
inputs, generatedFV/APRIORI and byte-exactAndroidv3 BootShim+FD packaging.
Retest preinsertedSD, removal/reinsert and `map -r`; hardware SD completion
remains unconfirmed. The previous40image is backed up under the same directory.

## 2026-10-09 Runtime90 current test image

The normal HDK8450 image is `Build/WaipioCrPkg/qcom-hdk8450.img`, built from
`.venv` with `-d qcom-hdk8450 -t DEBUG -s 0` at04:00:46 (+0800).
SHA256: `877f43f36e4d72b0f174042fddfcb0de791c504726177cbad34641117dad5316`;
size2031616bytes. It retains the stopped exclusive coherent UFS physical
DMA handoff for the stock Windows StorUfs driver, PCIe coherent translation,
and the legacy device-mode physical handoff used by working USB KDNET.
Physical Windows UFS completion and PCIe enumeration/MSI remain unverified.

Portable retained RPMh READ/own-DRV observation is now integrated. Native
provider publication remains a later unit. The final build verifies the actual
four-TU library, including `rpmh_rail_observe.obj`, after correcting an older
BaseTools timestamp-based source list. Five actual native FV modules, Androidv3
BootShim/gzipFD, UART packaging and the official MU boundary check pass.
EFI/NT45cases/config, original write regressions and Windows adapter lifecycle
tests pass. Current ARM64 SDK122C+1ASM, RpmhCr24C and PEP159C+2ASM link.
None of these checks proves physical rail output or installed-driver behavior.

The separate generated CR image remains
`Build/WaipioCrPkg/qcom-hdk8450-windows-cr.img` SHA256
`a14e8282fc0da7d406ae905a056c1a4f84785a741a881ce3eb0b747024b45887`.
Its ten non-DSDT tables use the local AcpiTableGenerator `sm8450_cr` profile;
see `Platforms/WaipioCrPkg/WindowsAcpi.md`. The SM8750 MBI workaround in
`pcie.md` is not a qualified SM8450 interrupt route.

Test the normal image with `fastboot boot` and capture Windows UFS startup
and native buttons/SPMI operation. The exact source/build evidence is in
`.tmp/waipio-continuation/runtime90-root/artifacts.json`.


### Runtime91 current integration image (2026-10-09)

Normal HDK8450 DEBUG, `-s 0`, built using `.venv`:
`Build/WaipioCrPkg/qcom-hdk8450.img`, SHA256
`483cbf5e6e1e7e6bb5763e0f0f87a7c9985a2572b68251b6288b321f9ec7067e`
(2031616 bytes, build finished 04:21:51). Before image877f43f3 is preserved under
`.tmp/waipio-continuation/runtime91-root/native-final/qcom-hdk8450-before.img`.
The optional generated Windows-CR imagea14e8282 remains separate.

Current GPU GMU/HFI service, owned IRQ/work retirement and named GPU interconnect
boot-floor units compile and pass host concurrency/failure tests. The native target
graph includes the tenth GPU provider. GPU execution is not initialized by
Start/DiagnosticStart; CP submission, HAL bank/PAS13 admission, normal slumber/wake
and rendering remain under development. This firmware preserves the exclusive
coherent UFS physical EBS handoff and USB KDNET device handoff.

Test the normal image with `fastboot boot Build/WaipioCrPkg/qcom-hdk8450.img`,
then installed Windows UFS boot and button input. Save a new full serial/EBS log
for an actual outcome. Host tests and image/FV checks do not establish hardware
UFS/GPU/PCIe success. Validation records live under
`.tmp/waipio-continuation/runtime91-root/`; prior failed harness/partial-receipt
outputs remain preserved separately from final successful records.

## Runtime92 complete PCIe target and configuration owner

The SM8450 PCIe descriptor is now constant and no longer uses lazy first-call
initialization. Native firmware uses its separate EFI backend. The NT default
refuses without touching output or issuing physical MMIO. The retained Windows
configuration owner is available through `Common/CrPcieConfigWdm.props`, which
imports the portable core, complete target and OSKAL memory backend. It accepts
port 0 and PASSIVE_LEVEL callers. A real host source, native retirement authority
and PCI.sys frontend remain separate work.

Current canonical and SDK builds each pass 19 configuration schedules in Debug
and Release. Native cold initialization, endpoint power and GPIO regressions
pass. Installed ARM64 EFI/WDK headers compile 16 complete objects; two imported
five-source proof links retain every object and pass, using logging-only WPP
substitution. The SDK export contains 71 libraries and 404 inputs and compiles
124 C units plus one assembler unit. These are software checks and
noninstallable proof binaries.

The normal virtualenv DEBUG `-s 0` firmware is
`Build/WaipioCrPkg/qcom-hdk8450.img`, 2,031,616 bytes, SHA256
`7c5444e0ad8bc3d797d38ec1e4bc47da16388fbc1f3a1f56510e30905799e74b`.
Actual `CrPcieTarget`/`CrPcieBackend` objects and the target library are bound to
the build. The coherent UFS physical EBS and KDNET handoffs remain included.
The optional generated CR88 image `a14e8282` is preserved.

Validation is recorded in
`.tmp/waipio-continuation/runtime92-root/artifacts.json` (`c5b76980`), with
1,133 current inputs and 592 outputs. The 4,577 conservative native source
inputs are separately archived. Two earlier PHY selections were restored to
live drafts after the build. Hardware validation and Windows PCIe/MSI
completion remain outstanding.

The separate collection audit in
`.tmp/waipio-continuation/runtime92-independent/verdict.md` is
`PASS_WITH_LIMITS`: current hashes, both input archives, source imports and
proof boundaries match. It adds no hardware or Windows driver activation
qualification.

## Runtime93 Windows CP integration

The A730.1 CP library and its connected Windows execution child are registered
and exported. The 27 guarded paths retain the independently reviewed engine,
SQE ingestion, HAL/MDL mappings, typed secure loan and parent stop/release
ordering. A failed Secure.End keeps the exact domain and pages alive; final
release retires the CP child before the GMU parent. Full GPL-2.0 text and source
provenance accompany the portable library.

Current canonical and SDK host core tests pass in four EFI/NT modes each.
The actual 42-unit WDM composition passes 15 cases in Debug and Release.
The complete current GpuCr import graph compiles 44 ARM64 C objects and links;
four additional EFI ARM64 CP objects also compile. Managed SDK export has
72 libraries and 414 files; its 126 C units and one assembler unit pass the
noninstallable compile/link probe. These checks use current production bytes,
with controlled OS/HAL/remote endpoints and installed kit header case mapping.

Evidence is bound in `.tmp/waipio-continuation/runtime93-root/artifacts.json`
(`62748d50`): 1,215 current inputs, 780 outputs and 27 source paths. The
independent code review is `gpu91-cp90-native-peer/review.md` (`abd495dc`).
Actual CP/GMU SID aliases, PAS13, secure aperture and native retirement still
need a live producer; RenderReady remains FALSE. Normal slumber/wake,
rendering and arbitrary-failure unload are incomplete. No CP class is selected
by native firmware; the delivered Native92 normal and CR88 images retain
their previous hashes and were not rebuilt for this Windows-only integration.

## Native94 ACPI generation and FACS preservation

The Windows CR profile uses `/home/sunf/AcpiTableGenerator`'s `sm8450_cr`
configuration for IORT and all nine other non-DSDT tables. DSDT stays in the
CrWDDK ASL workflow. The complete manifest selects all eleven tables together;
the build checks their actual ordered FV RAW bytes. See the build commands in
`Platforms/WaipioCrPkg/WindowsAcpi.md`.

An independent installation-path check found that the local ACPI installer
also recalculated the FACS checksum. FACS has no description-header checksum;
that write changed its HardwareSignature. The local Crane override now skips
FACS and preserves every byte passed to InstallAcpiTable. Ordinary table
checksum repair, read/install error handling and buffer cleanup remain intact.
The original failing fixture and its unchanged sixty successful fixed cases
are retained. Nine existing termination checks also pass. Official MU
functional sources remain at their pinned versions.

Both DEBUG `-s 0` images were rebuilt with `.venv/bin/activate`:

| Image | SHA256 |
|---|---|
| `Build/WaipioCrPkg/qcom-hdk8450.img` | `f197ea22516c9838a57594aedc296dc9fdfc64fc1f200fdacecbfc3d3dc63a04` |
| `Build/WaipioCrPkg/qcom-hdk8450-windows-cr.img` | `2387a6ab1de74186d4075b49d8460f2bc5907a70c9dd22ee39ea5126cbcb22ae` |

Both are 2,031,616 bytes. The ordinary image retains the current board/default
Windows policy and thirteen RAW sections; CR retains its generated eleven
tables plus three TPM sections. The normal coherent UFS physical handoff and
CR blocked-DMA handoff remain distinct. The two unreviewed live PHY drafts
were preserved; these builds use the previously qualified PHY selection.

Root evidence is `.tmp/waipio-continuation/runtime94-acpi/artifacts.json`
(`8c933e2c`). Independent reviews are `acpi93-current-peer/review.md`
(`47f59170`) and `acpi94-facs-package-peer/review.md` (`5c120e9b`). The latter
checks the actual source, object IR, linked ARM64 branch, single FV module and
image packaging. Hardware testing remains required.

The supplied `pcie.md` findings concern SM8750. Its MBI addresses, USB SID and
PHY values do not replace SM8450 data. UFS ACPI/IORT CCA is one, and native
UFS/PCIe streams use the qualified coherent policy. CR MCFG remains empty and
the namespace has no Windows PCI root; PCIe enumeration/configuration/MSI
integration is still incomplete. This installer fix does not establish the
cause of the reported UFS or PCIe symptoms.

## UFS100: request identity correction and test image (2026-10-09)

Latest `waipiolog.txt` (`c8e8015d...`) reaches HS-G4 Rate B with two lanes, then the first 96-byte INQUIRY reports initial OCS F and an empty response. Its `tag=0` is from the empty response, not the submitted request. The tested image hash is not recorded.

The Crane UFS fork now binds NOP/Query/SCSI task tags to the reserved UTRL slot, following open-source Android `e93f69dc83e69e5139e6a7911041f357b5e2b304` and Linux `40288c9206c17eb66a603262e06a58d300d0f279` ufshcd. A delayed outstanding command plus 300 subsequent requests demonstrated one active duplicate tag with the old UINT8 counter; the corrected implementation has zero duplicates. Responses must echo the request tag and SCSI LUN. Failed SCSI completion now records the submitted identity, descriptor and 12 HCI registers before cleanup. SMMU/PHY/power policy is unchanged.

Eight expanded UFS ASan/UBSan suites and the request-resource suite pass. Normal `.venv` build `./build_uefi.py -d qcom-hdk8450 -t DEBUG -s 0` completed at 17:54:10, October9. `Build/WaipioCrPkg/qcom-hdk8450.img`: 2043904 bytes, SHA256 `419d3dd0ab909b46fa5b87dcc60fcf0bac3ac0b4042d5fe7df5e40f3c571a190`. Verified image/FD/FV equality and actual fork UFS payload. Two shared PHY drafts were restored after using the established Native73 build selection; CR image `65bf0765...` remains unchanged.

This is a source/host/build correction, not proof that the original physical INQUIRY failure is resolved. Fresh board enumeration remains pending. Full evidence, input archive, original/fixed regression logs and preservation receipt: `.tmp/waipio-ufs100/report.md` and `build-receipt.json`. Only project source and open-source Android/Linux were implementation references; edk2-msm and leaked Qualcomm BSP source are excluded.
