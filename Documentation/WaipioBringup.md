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
Gen2 board limit come from the HDK DTS; PCIe1 allows Gen4. The local Linux tree
does not supply an MTP board DTS. MTP currently shares this description and
requires board verification of its rails, GPIO wiring, and link limits.
The PCIe0 wireless endpoint's separate PMU/firmware power sequence is outside
the root-complex/PHY initialization implemented here.

## Cold initialization and ownership

`CrDALDxe` publishes device data before the resource drivers run. See
[CrDAL.md](CrDAL.md) for the provider and lifetime contract.

| Resource | Owner |
| --- | --- |
| CMD DB metadata | `CmdDBCrDxe` |
| LDO requests | `RpmhCrDxe` clients of native NPA/PMIC resources |
| NoC bandwidth requests | `ICBCrDxe` clients of native NPA `/icb/arbiter` |
| RPMh TCS and BCM programming | Packaged Qualcomm RPMh/ICB drivers |
| GDSC, GCC clocks and resets | `ClockCrDxe` adapter to the packaged Clock protocol; see [ClockOwnership.md](ClockOwnership.md) |
| PERST and wake GPIOs | `GpioCrDxe` |
| PCIe DMA streams and page tables | `SmmuCrDxe` / `SmmuLib` |
| QMP PHY, PARF, DBI and iATU | `PcieLib` called by `PciHostBridgeLib` |

For each root complex, initialization asserts PERST, applies the LDO voltage
and HPM mode before enabling the rails, enables its power domain, acquires
ICB paths, attaches its SMMU streams, enables non-PIPE clocks, and sequences
controller and PHY resets. QMP power-control bits and register tables are
programmed before the PIPE mux/gates are enabled. PHY SW_RESET release and
START follow those clocks. Only after PHY readiness, DBI/iATU/SID configuration,
and the required reset delay does initialization release PERST and start LTSSM.

Native resource protocols preserve one hardware owner and aggregate requests
with existing firmware clients. In particular, native RPMh handles alone do
not aggregate votes; independent PMIC NPA and ICB NPA clients are necessary.
The packaged RPMh driver exclusively programs DRV2 TCS registers. The Crane
adapters must not initialize those registers or independently program BCMs
and GCC behind the native owners. The PMIC resource identities were verified
in both boards' DAL configuration; see
[WaipioPmicResources.md](WaipioPmicResources.md).

`ICBCrDxe` maps the Linux provider-local path IDs to Qualcomm's stable native
ICB identifiers as follows:

| Crane path | Native `/icb/arbiter` pair |
| --- | --- |
| PCIe0 memory `0x1000 -> 0x1100` | `MASTER_PCIE_0 (65) -> SLAVE_EBI1 (0)` |
| PCIe0 CPU/config `0x2000 -> 0x2100` | `MASTER_APPSS_PROC (0) -> SLAVE_PCIE_0 (84)` |
| PCIe1 memory `0x1001 -> 0x1101` | `MASTER_PCIE_1 (66) -> SLAVE_EBI1 (0)` |
| PCIe1 CPU/config `0x2000 -> 0x2101` | `MASTER_APPSS_PROC (0) -> SLAVE_PCIE_1 (85)` |

The Crane interconnect API follows Linux and expresses average and peak
bandwidth in kB/s, where one kB/s is exactly 1000 bytes/second. Native ICB
Type 3 requests use bytes/second, so the adapter rejects multiplication
overflow and converts both values by 1000. For example, the PCIe memory peak
vote of `250000` becomes `250000000` bytes/second and the CPU/config peak vote
of `1000` becomes `1000000` bytes/second.

Each path has its own `NPA_CLIENT_VECTOR` handle. Release sends a synchronous
zero Type 3 vector before destroying that handle; it does not use
`CompleteRequest`, whose packaged implementation may return before a
fire-and-forget request reaches hardware. A failed zero vote or destroy keeps
the path owned so teardown can retry without losing the client handle.

Each port records acquired resources. Teardown fences the endpoint before
revoking SMMU mappings, then releases dependent resources. Shared rail/clock
references prevent one port's failure from dropping the other port's vote.
A failed release retains ownership and prerequisites for a later retry.
Voltage and mode requests are included in rollback even if rail enable was
never reached. An incomplete shared acquisition cannot satisfy another port.

The Qualcomm HAL binary continues to serve other firmware clients. It does
not implement the standard EDKII IOMMU protocol. Its PCIe SID ownership rules
and static-analysis limits are documented in [SmmuOwnership.md](SmmuOwnership.md).

## QUP, GPI, I2C and SPI

The packaged MU/Qualcomm QUP, GENI and GPI implementations retain hardware
ownership. `QUPCrDxe` publishes the Linux-derived wrapper/engine descriptions;
`GpiCrDxe` forwards the private GPI ABI. `I2CCrDxe` and `SPICrDxe` adapt the
packaged private protocols, and use standard PI master/host-controller
protocols when available. They do not reinitialize controllers behind those
owners.

The private I2C adapter implements repeated START and completion checks. Its
event API defers a synchronous private transfer to a TPL_CALLBACK worker.
The SPI adapter supports the private backend's equal-length full-duplex
transaction format and reports unsupported standalone CS/clock operations.
The public Crane protocol prefixes remain compatible; extensions increase
the revisions. The private ABI declarations are in `EFIMuBusProtocol.h`.

## Validation boundary

The GPI/I2C/SPI adapters and MU host bridge use normal dependency dispatch;
they are excluded from APRIORI so missing backend/resource protocols cannot
be bypassed.

Host tests compile the production library/adapter sources with EDK2 headers,
MMIO/protocol mocks, and address/undefined-behavior sanitizers. They cover
resource ordering, shared-resource rollback, ICB votes, SMMU programming,
CrDAL data contracts, bus transfer semantics, and interrupt callback lifetime.
Run the `Tests/run_*_tests.py` runners from the enclosing firmware workspace
with `TMPDIR=/var/tmp` if `/tmp` is constrained.

Portable `RpmhLib`, `ClockLib`, and `ICBLib` remain available for environments
where Crane owns the hardware. Their MMIO tests cover bounded polling,
command completion, shared references, and vote computation. Waipio's DXE
adapters use native protocols instead; those portable tests do not establish
the behavior of the packaged firmware drivers.

Firmware builds validate compilation, linkage, dependency expressions, and
FFS packaging. They do not establish PCIe link training, endpoint operation,
or hardware DMA isolation. Those require boot logs and device tests on each
board, including failure/retry and the HAL coexistence checks above.
