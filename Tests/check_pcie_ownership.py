#!/usr/bin/env python3
"""Check that only MU PciHostBridgeDxe owns the Crane cold-init library."""
from pathlib import Path
import re

crane = Path(__file__).resolve().parents[1]
platforms = crane.parent
host = "MdeModulePkg/Bus/Pci/PciHostBridgeDxe/PciHostBridgeDxe.inf"
direct = "CranePkg/Library/PciSegmentLib/PciSegmentLib.inf"
protocol = (
    "MdePkg/Library/UefiPciSegmentLibPciRootBridgeIo/"
    "UefiPciSegmentLibPciRootBridgeIo.inf"
)


def active_lines(path):
    return [line.split("#", 1)[0].strip() for line in path.read_text().splitlines()]


defaults = active_lines(crane / "Crane.dsc.inc")
assert f"PciSegmentLib|{protocol}" in defaults, "PCI consumers bypass RootBridgeIo"
assert f"PciSegmentLib|{direct}" not in defaults, "cold-init owner is global"

checked = 0
for device in sorted((platforms / "WaipioPkg/Device").glob("*/DXE.dsc.inc")):
    lines = active_lines(device)
    if not any(line.startswith(host) for line in lines):
        continue
    owner = None
    direct_owners = []
    for line in lines:
        if line.endswith("{") and ".inf" in line:
            owner = line[:-1].strip()
        elif line == "}":
            owner = None
        elif line == f"PciSegmentLib|{direct}":
            direct_owners.append(owner)
    assert direct_owners == [host], f"{device}: direct PCI path lacks one host owner"
    checked += 1
assert checked >= 2, "Waipio host-bridge configurations were not checked"

# The direct library is a private dependency of the host's config accessor;
# protocol consumers must not pull a second cold-init context into their image.
direct_inf = "\n".join(active_lines(crane / "Library/PciSegmentLib/PciSegmentLib.inf"))
assert re.search(r"(?m)^PciHostBridgeLib$", direct_inf)
host_inf = "\n".join(active_lines(crane / "Library/PciHostBridgeLib/PciHostBridgeLib.inf"))
depex = host_inf.split("[Depex]", 1)[1]
required = {
    "gEfiCrDalProtocolGuid",
    "gEfiClockCrProtocolGuid",
    "gEfiGpioCrProtocolGuid",
    "gEfiInterconnectCrProtocolGuid",
    "gEfiRpmhCrProtocolGuid",
    "gEfiSmmuCrProtocolGuid",
}
assert set(re.findall(r"g\w+Guid", depex)) == required
assert " OR " not in depex and "TRUE" not in depex
for board in ("qcom-hdk8450", "qcom-mtp8450"):
    apriori = active_lines(platforms / "WaipioPkg/Device" / board / "APRIORI.inc")
    assert f"INF {host}" not in apriori, f"{board}: APRIORI bypasses host dependencies"
adapters = ("RpmhCrDxe", "ClockCrDxe", "GpioCrDxe", "ICBCrDxe", "QUPCrDxe",
            "GpiCrDxe", "I2CCrDxe", "SPICrDxe")
apriori = "\n".join(active_lines(crane / "Crane.Apriori.inc"))
for adapter in adapters:
    assert f"Driver/{adapter}/" not in apriori, f"{adapter}: dispatches before MU backend"

# The packaged Qualcomm drivers already own GCC, RPMh TCS and BCM state.
# Linking the portable hardware owners into these adapters would allow two
# independent clients to reinitialize or overwrite the same registers.
for adapter, raw_lib in (("ClockCrDxe", "ClockLib"),
                         ("RpmhCrDxe", "RpmhLib"),
                         ("ICBCrDxe", "ICBLib")):
    path = crane / "Driver" / adapter / f"{adapter}.inf"
    lines = active_lines(path)
    assert raw_lib not in lines, f"{adapter}: links a second hardware owner"
    depex = "\n".join(lines).split("[Depex]", 1)[1]
    assert "gEfiCrDalProtocolGuid" in depex
    assert " AND " in depex and "TRUE" not in depex and " OR " not in depex
print(f"Crane PCI ownership: {checked} targets route consumers through MU RootBridgeIo")

# PciHostBridgeLib runs before MU creates the BAR apertures. Its controller,
# QMP and translated I/O windows must already be accessible as device memory.
required_windows = [
    (0x01C00000, 0x3000), (0x01C06000, 0x2000),
    (0x01C08000, 0x3000), (0x01C0E000, 0x2000),
    (0x40000000, 0x2000), (0x40100000, 0x200000),
    (0x60000000, 0x2000), (0x60100000, 0x200000),
]
for board in ("qcom-hdk8450", "qcom-mtp8450"):
    path = (platforms / "WaipioPkg/Device" / board /
            "Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c")
    regions = [(int(base, 16), int(size, 16)) for base, size in re.findall(
        r'\{\s*"[^"]+",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),'
        r'\s*AddDev,\s*MMAP_IO,\s*UNCACHEABLE,\s*MmIO,\s*NS_DEVICE',
        path.read_text())]
    for base, size in required_windows:
        assert any(start <= base and base + size <= start + length
                   for start, length in regions), f"{board}: unmapped PCIe {base:#x}"
print("Crane PCI mappings: both targets cover cold-init and translated I/O windows")
