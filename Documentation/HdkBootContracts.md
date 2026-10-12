# HDK8245/HDK8350 boot contracts (2026-10-12)

The updated board logs reach native GOP publication (1080x2340 command mode and
1920x1080 LT9611 video respectively). They do not establish correct subsequent
console drawing or visible HDMI/panel output. UFS and HDK8350 ADSP/UCSI also reach
successful initialization; early NOT_READY messages are not permanent failures.

## Repairs

* Native GOP QueryMode/SetMode/Blt now accept TPL_NOTIFY, as used by the MU
  RenderingEngine and GraphicsConsole. Command frame publication only cleans
  the already-owned auto-refresh buffer at this priority. Existing DMA, owner,
  generation, serialization and ExitBootServices checks remain mandatory.
  Startup, shutdown, power and brightness retain their CALLBACK ceiling.
* The platform must package GraphicsConsoleDxe and wait for the exact native
  override protocol before dispatching MsEarlyGraphics. Each early-graphics
  caller receives an owned mode snapshot; repeat calls refresh it in place.
  Failure does not leak allocations, return an unset output or change timing.
* Native HDK console policy preserves fixed panel timing and chooses a real
  negotiated text mode rather than requiring VGA or a guessed native text grid.
  It still requires GOP on the actual ConsoleOutHandle and propagates failures.
* The RAM-backed WHEA byte store qualifies its mapping for Read, Write and Clear.
  Capacity, address and range checks reject underflow/truncation and bad mappings.
  No new persistent storage or blanket clearing of invalid inherited records is
  introduced; a first-boot invalid WHEA checksum is not claimed repaired.
* Renesas 1912:0014/0015 firmware admission runs before xHCI reset. FW=00/ROM=0000
  is not a working controller: defer admission rather than repeat long resets.
  Completed ROM or firmware load is accepted; PCI errors retain exact cleanup.
  Public reference: Linux drivers/usb/host/xhci-pci-renesas.c firmware and ROM
  result checks. No firmware download, ROM write or invented payload is added.

## Validation and remaining board evidence

`Tests/run_hdk_boot_contract_tests.py` compiles actual production translation
units with modeled EFI services under Clang ASan/UBSan. The command/video GOP
and typed-resource composition suites cover NOTIFY, HIGH, mapping loss,
retained cleanup and power-control restrictions. XhciStartTest covers firmware
preflight and its existing actual Start/Stop ownership checks. The platform
console configuration test and DEBUG workflow bind the two repositories.

Hardware remains the acceptance boundary. Verify console GOP, visible text/UI,
boot graphics, touch/pointer drawing, and ExitBootServices on both boards.
The HDK8245 Renesas endpoint still needs valid external firmware. Its port-1
PCIe no-signal timeout alone cannot distinguish an absent endpoint from a board
problem. The two HDK8350 FatDiskIo errors have no controller/LBA attribution;
passing NVMe host tests does not resolve them. Missing EFI boot files and absent
network drivers are not fixed by ignoring authentication or claiming I/O success.
