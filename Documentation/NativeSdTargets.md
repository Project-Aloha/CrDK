# Native SDCC2 target contract

`CrTargetLib` publishes immutable instance zero `CrDalDeviceSd` (appended ID22).
`CrDalGetSdContext` validates the protocol result, descriptor revision/size,
v5 topology, complete clocks/rails/paths and pin fields before returning it.
An absent descriptor leaves SD methods unsupported. The native GPIO driver
keeps working for unrelated pins. There is no SoC inference from pin count,
controller address or a Windows ACPI tuple.

Both native drivers borrow the same permanent target storage through boot
services. `GpioCrDxe` resolves its real GPIO addresses without MMIO, then
validates every selected normal/special-pad descriptor before PDC/IRQ setup.
It caches that exact target and fences its CD and all three special pads in
configuration, output and IRQ entry points. Its SD callback returns raw HIGH;
`SdResources.c` applies the target polarity. `SdhciCrDxe` validates the whole
target before creating its EBS event, and takes the described ICC paths,
AHB/APPS clock references, supplies and power interrupt. Partial effects and
failed reverse cleanup remain in the existing resident owner journal.

| Native profile | CD/present polarity | Special pads/offset | Power IRQ | Supplies |
|---|---|---|---|---|
| HDK8450 |92/HIGH |211..213/0x1d6000 |255 level high |ldoc9,ldoc6:3008mV/HPM7 |
| HDK8250 |77/LOW |181..183/0x9b7000 |254 level high |ldoc9,ldoc6:2960mV/HPM7 |

The shared electrical policy is CLK no pull, CMD/DATA pull-up, active16mA,
sleep2mA, and normal CD input/pull-up/2mA. Native profile helpers require the
owner to retain the same immutable profile through snapshot and restore.
The existing SM8450 combined API and Windows pads-only API keep their
signatures, snapshot layouts and fixed defaults; Windows pads-only operations
never access CD metadata or CD registers.

PIO preparation now explicitly clears and reads back vendor3(+0x250)
`CORE_FIFO_ALT_EN` bit10 together with the existing DLL power-save bit3,
using a masked write, barrier and exact-success IO. It preserves neighbouring
bits and refuses a live data transfer. A reset completion is not a witness of
FIFO selection. Dropped writes, a read-only alternate-FIFO bit and nonfinal
IO statuses cannot report successful preparation. The host and Windows aux
owner perform this preparation before their supported SDR/PIO request path.

The register facts are from Android `drivers/mmc/host/sdhci-msm.c` at SM8250
commit `cad09e061ef6cf689a4a1e54d27562e0f042236a` (bit10 definition142,
probe5180..5186), and SM8450 commit
`449760504b3a75728eb60479f5b34dc51cb263ce` (definition149,
probe4925..4928). Both v5/mci-removed offset tables put vendor3 at0x250.
The bounded preparation adds checked readback to these source-defined effects.
HDK8250's twelve-source board closure is reproduced by
`KonaCrPkg/Tools/generate_sd.py` and `Tests/sm8250-sd-facts.json`.
HDK8450 keeps its previously qualified Linux OPP votes and schematic CD quirk.

Validation commands are `Tests/run_sd_resources_tests.py --output <new-dir>`,
`run_sdhci_native_tests.py --output <new-dir>`,
`run_sdhci_mmc_integration_tests.py --output <new-dir>`,
`run_sdhci_pio_profile_tests.py --output <new-dir>` and
`run_crdal_tests.py --output <new-dir>`. The FIFO runner can compile a saved
old core with `--core-source <saved.c> --expect-before`; the DAL runner supports
saved `--client-source` and `--client-test` with `--expect-before`.
Receipts bind actual recursive compiler inputs, objects/binaries and logs.
Native core/MMC/resource models and Windows compatibility fixtures qualify
software behavior. Board SD-card enumeration, hotplug and sustained IO remain
physical validation; these tests grant no DMA/reset or hardware readiness claim.
