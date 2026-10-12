# SM8550 Crane SoC target

`Target/Sm8550/CrTargetLib.inf` provides the board-independent foundation for
the requested SM8550 native Crane work (`8550cr`). It contains a real CrDAL
provider and resource data for existing shared cores. It is not an active
platform/board configuration or a bootable firmware image. No device has been
chosen, no `KailuaCrPkg` board has been created, and no `sm8550cr` build entry
has been activated.

The source milestone adds immutable SoC identity and shared mutable TLMM,
PDC and RPMh contexts. Each mutable resource has one static instance owned by
the provider. The provider fills descriptors once before publication and
leaves their metadata unchanged on subsequent calls. Existing driver and
portable-library implementations perform the IO; the target adds no private
GPIO, PDC, UART or RPMh driver.

## Public sources

All hardware facts below come from public Linux revision
`a53261b60834ce617cc4f3c7723dda8285c59789`. Paths and lines refer to that
revision, not an unspecified vendor tree. Existing self-developed Crane/MU
source defines the resource and OSKAL contracts. No edk2-msm, leaked Qualcomm
BSP, nonpublic `Silicon/QC` BSP, `Binaries` or `PatchedBinaries` source was used.

| Resource | Public source | Implemented facts |
| --- | --- | --- |
| SoC identity | `include/dt-bindings/arm/qcom,ids.h:256`, `Documentation/devicetree/bindings/arm/qcom.yaml:1139` | Qualcomm SoC ID 519, public compatible `qcom,sm8550` |
| TLMM aperture/IRQ | `arch/arm64/boot/dts/qcom/sm8550.dtsi:4702` | `0x0f100000`, size `0x300000`, raw SPI208 / GSI240 |
| Pad/mux/registers | `drivers/pinctrl/qcom/pinctrl-sm8550.c:16` and `:1494` | GPIO0..209, UFS_RESET210, SDC2 pads211..213; ordinary mux availability and EGPIO ownership selector9 |
| Wake routes | `drivers/pinctrl/qcom/pinctrl-sm8550.c:1711` | All 93 GPIO-to-PDC mappings, including PDC138..142 |
| PDC | `arch/arm64/boot/dts/qcom/sm8550.dtsi:4622` | `0x0b220000/0x10000`, five ranges, SPI config window `0x174000f0/0x64` |
| RPMh | `arch/arm64/boot/dts/qcom/sm8550.dtsi:5766`, `drivers/soc/qcom/rpmh-rsc.c:1083` and `:1107` | DRV2 resource `0x17a20000/0x10000`, matching interrupt2 SPI5/GSI37, TCS offset `0xd00`, active/sleep/wake/control counts3/2/2/0 |
| Optional debug UART7 | `arch/arm64/boot/dts/qcom/sm8550.dtsi:2240` and `:5473` | GENI `0x00a9c000/0x4000`, SPI579/GSI611; GPIO26/27 resource candidates |

The PDC source `drivers/irqchip/qcom-pdc.c` is absent from the sparse local
working tree and was inspected with `git show HEAD:drivers/irqchip/qcom-pdc.c`.
It remains a public source at the same fixed revision.

`Target/Sm8550/Tools/generate_gpio.py` extracts hardware facts from the exact
public pinctrl source. It checks the revision and verifies the materialized
file against the committed blob before producing or checking the resource
table. It does not copy Linux driver implementation code.

## Identity and UART selection

The common schema appends `CrDalDeviceSocInfo=26` and `CR_DAL_SOC_INFO`, an
optional immutable payload with revision, size, public Qualcomm ID and a
terminated fixed-size compatible string. Existing device numbers and the
version1 descriptor stride are preserved. This is the declared resource
provider identity, not runtime hardware attestation.

The target INF specifies module default `PcdUartSerialBase=0`, which omits
the UART descriptor. An explicit future board override to `0x00a9c000`
advertises the inherited debug UART7. Other values return `EFI_UNSUPPORTED`
before manifest publication or MMIO, including the generic Andromeda default
`0x0098c000` from a different SoC. UART14/15 are normal GENI endpoints and
require clock/ICC admission before a future implementation can expose them.

The optional UART context describes a console already initialized by an
earlier boot stage. An explicit base alone does not establish initialized
clock, pin, FIFO, firmware, baud or cable state. That remains a board and
boot-stage contract. The standalone tests inject compile-time PCD constants;
they do not prove DSC override/AutoGen or console integration.

## Verification

Run from the workspace root, using DEBUG validation only:

```sh
python3 Platforms/CranePkg/Target/Sm8550/Tools/generate_gpio.py \
  --linux .vscode/linux --check --receipt .tmp/sm8550-cr/source-revision.json
python3 Platforms/CranePkg/Tests/run_sm8550_target_tests.py \
  --output .tmp/sm8550-cr/host
python3 Platforms/CranePkg/Tests/check_sm8550_target_arm64.py \
  --output .tmp/sm8550-cr/arm64-debug
```

The host runner builds the actual target provider, registry, GPIO, PDC and
RPMh cores with ASan/UBSan. A deterministic MMIO fixture checks manifest
identity/ABI and descriptor stability, all pad addresses and wake routes,
EGPIO ownership and invalid mux rejection, UFS reset access and unsupported
special-pad IRQs, PDC inverse routes, the new range, legacy enable-bank
initialization and 142/143 bounds, polarity conversion, SPI bit31 writes, and
RPMh DRV2 active-only initialization/IRQ mask and deinitialization. Separate
executables cover disabled, explicitly selected and unsupported UART bases.
It does not test real IRQ delivery, UART transport or remote RPMh completion.

The ARM64 checker compiles five real target files and five shared-core files
to ARM64 COFF objects with `-g -O0 -D_DEBUG=1`, actual MU headers and no host
MMIO/time replacements or `NDEBUG`. It verifies the COFF machine and writes
compiler commands, input hashes and object hashes. This is an object compile
check, with no linking, packaging or firmware/board execution.

The existing CrDAL, GPIO, PDC bounds, RPMh and native GPIO/PDC IRQ runners also
passed in this milestone. Task-local logs and receipts are under
`.tmp/sm8550-cr/`; the initial missing-target regression failure is retained
in `red.log`.

## Remaining source and board dependencies

- **GPIO electrical configuration:** public SM8550 pads have strong-I2C pull
  bit13. Existing `GpioConfigRegInfo` cannot describe that bit; ordinary pull
  changes preserve it and readback omits it. This target does not claim
  complete SM8550 pull configuration. A shared optional mask plus explicit
  strong-pull request/readback semantics must be implemented before that
  mode can be managed. The checked whole-pad interface also intentionally
  rejects UFS/SDC special pads; their basic APIs are separate.
- **PDC hardware revision and ownership:** public Linux reads version at
  `+0x1000` and supports v3.2 enable in IRQ_CFG bit3 as well as older enable
  banks. Current Crane uses the older bank layout. The resource table and
  host range/legacy-layout tests cannot establish which layout a future
  board has. Runtime version admission or shared version-aware handling is
  required before enabling board IRQs. Public DTS describes the second SPI
  aperture, but this Linux revision does not use it; whether Crane's extra
  SPIConfig writes are required/authorized also needs a public contract or
  board evidence. Initialization's ownership of inherited PDC routes must
  be established before running it on a board.
- **RPMh integration:** CmdDB carveout mapping/ownership, ARC and BCM metadata,
  rail inventories and shared votes are not advertised by this milestone.
  Clock, ICC and QUP adapters must consume actual SM8550 resources and their
  dependencies. Tested DRV2 geometry is not proof of a successful remote vote.
- **Other SoC paths:** GCC/TCSR/dispcc/gpucc clock variants, ICC nodes and BCM
  graphs, QUP engines and GPI, SMMU stream/context/ownership, storage/USB/PCIe
  PHY resources, display, firmware services, PMICs and board rails remain
  separate source milestones. Missing devices are omitted from the manifest.
- **Board activation:** memory map/carveouts and FDT, UART admission, PMIC and
  key wiring, panel/PHY/reset/rails, ACPI, runtime MMIO/IRQ ownership, packaged
  DEBUG firmware and boot testing require a device choice and public board
  evidence. No board image or board-test readiness is asserted here.
