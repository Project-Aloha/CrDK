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
for device in sorted((platforms / "WaipioCrPkg/Device").glob("*/DXE.dsc.inc")):
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
assert checked >= 1, "HDK8450 host-bridge configuration was not checked"

# HDK8245/8350 use the same single MU producer ownership contract.
for package,board,ports in [("KonaCrPkg","qcom-hdk8245",3),("LahainaCrPkg","qcom-hdk8350",2)]:
    device=platforms/package/"Device"/board
    lines=active_lines(device/"DXE.dsc.inc");owner=None;owners=[]
    for line in lines:
        if line.endswith("{") and ".inf" in line:owner=line[:-1].strip()
        elif line=="}":owner=None
        elif line==f"PciSegmentLib|{direct}":owners.append(owner)
    assert owners==[host],str(device)
    assert f"INF {host}" in active_lines(device/"DXE.inc")
    assert f"INF {host}" not in active_lines(device/"APRIORI.inc")
    assert f"PciSegmentLib|{protocol}" in active_lines(platforms/package/"Include/Crane.dsc.inc")
    mapping=(device/"Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c").read_text()
    regions=[(int(base,16),int(size,16)) for base,size in re.findall(
        r'\{\s*"[^"]+",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*AddDev,\s*MMAP_IO,\s*UNCACHEABLE,\s*MmIO,\s*NS_DEVICE',mapping)]
    for port in range(ports):
        for base,length in [(0x1c00000+port*0x8000,0x8000),[(0x60000000,0x4000000),(0x40000000,0x20000000),(0x64000000,0x4000000)][port]]:
            assert any(a<=base and base+length<=a+b for a,b in regions),(board,hex(base))
    for dsc in (platforms/package).glob("*.dsc"):
        text=dsc.read_text();assert text.index("!include MUCommonCrPkg/MUCommonCr.dsc.inc")<text.index("gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|32"),str(dsc)
    checked+=1
source=(crane/"Library/PciHostBridgeLib/PciHostBridgeLib.c").read_text()
adapter=source[source.index("InitializeResourceAdapters ("):source.index("ReleasePciePortMask (")]
assert adapter.index("Status = EnsurePcieBootFloor ()")<adapter.index("mPcieIo.SetClock")
initialize=source[source.index("CranePcieHostInitialize ("):]
assert initialize.index("InitializeResourceAdapters ()")<initialize.index("PcieLibInit (")

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
    apriori = active_lines(platforms / "WaipioCrPkg/Device" / board / "APRIORI.inc")
    assert f"INF {host}" not in apriori, f"{board}: APRIORI bypasses host dependencies"
adapters = ("RpmhCrDxe", "ClockCrDxe", "GpioCrDxe", "ICBCrDxe", "QUPCrDxe",
            "GpiCrDxe", "I2CCrDxe", "SPICrDxe")
apriori = "\n".join(active_lines(crane / "Crane.Apriori.inc"))
for adapter in adapters:
    assert f"Driver/{adapter}/" not in apriori, f"{adapter}: dispatches before MU backend"

# Crane's clock, RPMh and interconnect adapters are the native hardware
# owners.  They must link their standalone backends and must not fall back to
# the packaged Qualcomm clock protocol.
for adapter, raw_lib in (("ClockCrDxe", "ClockLib"),):
    path = crane / "Driver" / adapter / f"{adapter}.inf"
    lines = active_lines(path)
    assert raw_lib in lines, f"{adapter}: lacks its standalone hardware owner"
    assert "gEfiClockProtocolGuid" not in lines, \
        f"{adapter}: consumes the legacy Qualcomm clock protocol"
    depex = "\n".join(lines).split("[Depex]", 1)[1]
    assert "gEfiCrDalProtocolGuid" in depex
    # ClockCrDxe owns the native backend and only needs CrDAL. A legacy clock
    # protocol dependency would reintroduce the packaged Qualcomm owner.
    assert "TRUE" not in depex and " OR " not in depex
    assert set(re.findall(r"g\w+Guid", depex)) == {"gEfiCrDalProtocolGuid"}
for adapter, raw_lib in (("RpmhCrDxe", "RpmhLib"),
                         ("ICBCrDxe", "ICBLib")):
    path = crane / "Driver" / adapter / f"{adapter}.inf"
    lines = active_lines(path)
    assert raw_lib in lines, f"{adapter}: lacks its standalone hardware library"
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
    path = (platforms / "WaipioCrPkg/Device" / board /
            "Library/PlatformMemoryMapLib/PlatformMemoryMapLib.c")
    regions = [(int(base, 16), int(size, 16)) for base, size in re.findall(
        r'\{\s*"[^"]+",\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),'
        r'\s*AddDev,\s*MMAP_IO,\s*UNCACHEABLE,\s*MmIO,\s*NS_DEVICE',
        path.read_text())]
    for base, size in required_windows:
        assert any(start <= base and base + size <= start + length
                   for start, length in regions), f"{board}: unmapped PCIe {base:#x}"
print("Crane PCI mappings: both targets cover cold-init and translated I/O windows")

# The common Andromeda PCD include resets the CPU I/O HOB width to zero.  The
# HDK target must override it after that include so the translated 1 MiB I/O
# apertures at 0x40200000 and 0x60200000 are present in the DXE GCD map.
post_pcd = (platforms / "WaipioCrPkg/Device/qcom-hdk8450/PcdsPostMUCommonCr.dsc.inc")
assert "gEmbeddedTokenSpaceGuid.PcdPrePiCpuIoSize|32" in post_pcd.read_text()
for dsc_name in ("WaipioCr.dsc", "WaipioCrNoSb.dsc"):
    dsc_text = (platforms / "WaipioCrPkg" / dsc_name).read_text()
    common = dsc_text.index("!include MUCommonCrPkg/MUCommonCr.dsc.inc")
    override = dsc_text.index("PcdsPostMUCommonCr.dsc.inc")
    assert common < override, f"{dsc_name}: CPU I/O PCD override precedes Andromeda"
print("Crane PCI GCD: HDK CPU I/O HOB width override follows Andromeda defaults")
