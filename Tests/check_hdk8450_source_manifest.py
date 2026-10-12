#!/usr/bin/env python3
"""Reject stock HDK8450 hardware driver entries in active device manifests.

Raw panel/config files under Resources are firmware data fixtures and
are allowed. Any INF, PE32, or FILE DRIVER that resolves through Binaries or
PatchedBinaries would reintroduce a stock hardware owner.
The separately requested X86EmulatorPkg is an explicit upstream binary import.
"""
from pathlib import Path
import argparse
import re
import struct
import uuid
import hashlib
import json

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument(
    "--build-dir", type=Path,
    help="verify the FV from this specific build (for example Build/WaipioCrPkg/DEBUG_CLANGPDB)",
)
args = parser.parse_args()

root = Path(__file__).resolve().parents[2]
device = root / "WaipioCrPkg/Device/qcom-hdk8450"
fdf = root / "WaipioCrPkg/WaipioCr.fdf"
manifests = [device / name for name in ("APRIORI.inc", "DXE.inc", "DXE.dsc.inc")]
violations = []
for manifest in manifests + [fdf]:
    lines = manifest.read_text().splitlines()
    for number, original in enumerate(lines, 1):
        line = original.split("#", 1)[0].strip()
        if not line:
            continue
        if "PatchedBinaries/" in line:
            violations.append((manifest, number, original))
        elif "Binaries/" in line:
            violations.append((manifest, number, original))
if violations:
    for path, number, line in violations:
        print(f"{path}:{number}: {line}")
    raise SystemExit("HDK8450 active manifest contains a precompiled driver")

# APRIORI entries are scheduled without evaluating their DEPEX by the MU DXE
# dispatcher. Keep the complete native provider-to-consumer chain explicit so
# a generated APRIORI section cannot start a consumer before its protocol
# producer. The reference devices put BdsDxe before the late console tail;
# BdsEntry itself runs after DXE dispatch has drained that tail.
def active_lines(path):
    return [line.split("#", 1)[0].strip() for line in path.read_text().splitlines()]


apriori_path = device / "APRIORI.inc"
apriori_lines = active_lines(apriori_path)
crane_apriori = root / "CranePkg/Crane.Apriori.inc"
expanded_apriori = []
for line in apriori_lines:
    if line == "!include CranePkg/Crane.Apriori.inc":
        expanded_apriori.extend(active_lines(crane_apriori))
    else:
        expanded_apriori.append(line)

critical_order = [
    "INF CranePkg/Driver/CrDALDxe/CrDALDxe.inf",
    "INF CranePkg/Driver/CmdDBCrDxe/CmdDBCrDxe.inf",
    "INF CranePkg/Driver/SmemCrDxe/SmemCrDxe.inf",
    "INF CranePkg/Driver/RamPartitionCrDxe/RamPartitionCrDxe.inf",
    "INF CranePkg/Driver/MpServicesCrDxe/MpServicesCrDxe.inf",
    "INF CranePkg/Driver/RpmhCrDxe/RpmhCrDxe.inf",
    "INF CranePkg/Driver/ClockCrDxe/ClockCrDxe.inf",
    "INF CranePkg/Driver/GpioCrDxe/GpioCrDxe.inf",
    "INF CranePkg/Driver/ICBCrDxe/ICBCrDxe.inf",
    "INF CranePkg/Driver/SmmuCrDxe/SmmuCrDxe.inf",
    "INF CranePkg/Driver/MdssHandoffCrDxe/MdssHandoffCrDxe.inf",
    "INF CranePkg/Driver/QUPCrDxe/QUPCrDxe.inf",
    "INF CranePkg/Driver/GpiCrDxe/GpiCrDxe.inf",
    "INF CranePkg/Driver/I2CCrDxe/I2CCrDxe.inf",
    "INF CranePkg/Driver/SpmiCrDxe/SpmiCrDxe.inf",
    "INF CranePkg/Driver/ButtonsCrDxe/ButtonsCrDxe.inf",
    "INF CranePkg/Driver/UfsHcCrDxe/UfsHcCrDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf",
    "INF MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf",
    "INF CranePkg/Driver/SdhciCrDxe/SdhciCrDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/EmbeddedPkg/Universal/MmcDxe/MmcDxe.inf",
    "INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf",
    "INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf",
    "INF MdeModulePkg/Universal/Disk/UnicodeCollation/EnglishDxe/EnglishDxe.inf",
    "INF FatPkg/EnhancedFatDxe/Fat.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe/VariableRuntimeDxe.inf",
    "INF CranePkg/Driver/MDSSCrDxe/MDSSCrDxe.inf",
    "INF CranePkg/Driver/Lt9611uxcCrDxe/lt9611.inf",
    "INF MdeModulePkg/Universal/BdsDxe/BdsDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Universal/SerialDxe/SerialDxe.inf",
    "INF MdeModulePkg/Universal/Console/TerminalDxe/TerminalDxe.inf",
    "INF MdeModulePkg/Universal/Console/ConPlatformDxe/ConPlatformDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Universal/Console/ConSplitterDxe/ConSplitterDxe.inf",
    "INF MdeModulePkg/Universal/Console/GraphicsConsoleDxe/GraphicsConsoleDxe.inf",
]
positions = {}
for number, line in enumerate(expanded_apriori, 1):
    positions.setdefault(line, number)
missing = [entry for entry in critical_order if entry not in positions]
if missing:
    raise SystemExit(
        "APRIORI.inc: missing native dispatch entries: " + ", ".join(missing)
    )
ordered_positions = [positions[entry] for entry in critical_order]
if ordered_positions != sorted(ordered_positions):
    raise SystemExit(
        "APRIORI.inc: providers, display, console, and BDS are out of order"
    )

# Do not dispatch the SPI adapter from APRIORI.  Its DEPEX intentionally has
# an OR branch for the available SPI protocol, and this target has no native
# producer that makes an APRIORI placement safe.
if "INF CranePkg/Driver/SPICrDxe/SPICrDxe.inf" in expanded_apriori:
    raise SystemExit("APRIORI.inc: SPICrDxe has an unresolved OR dependency")

# Crane now publishes the legacy inventory protocol and expands RAM itself.
# Do not also run the old hard-coded Andromeda expansion algorithm.
ram_partition = "INF MUCommonCrPkg/Driver/RamPartitionDxe/RamPartitionDxe.inf"
if ram_partition in expanded_apriori:
    raise SystemExit("APRIORI.inc: duplicate legacy RAM expansion")

# Every critical APRIORI entry must also be present in the source FV manifest;
# an APRIORI GUID without an FFS is silently ignored by the dispatcher.
emulator_pkg = root / "X86EmulatorPkg"
fdf_sources = [fdf, device / "DXE.inc", root / "CranePkg/Crane.fdf.inc", emulator_pkg / "X86Emulator.fdf.inc"]
fdf_entries = {line for path in fdf_sources for line in active_lines(path)}
for entry in critical_order:
    if entry not in fdf_entries:
        raise SystemExit(f"FV source manifest: APRIORI entry is not packaged: {entry}")

# Driver-binding layers do not need APRIORI placement, but omitting any one
# leaves storage without partition or filesystem handles.
storage_entries = [
    "INF CranePkg/Driver/SdhciCrDxe/SdhciCrDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/EmbeddedPkg/Universal/MmcDxe/MmcDxe.inf",
    "INF CranePkg/Driver/UfsHcCrDxe/UfsHcCrDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Ufs/UfsPassThruDxe/UfsPassThruDxe.inf",
    "INF MdeModulePkg/Bus/Scsi/ScsiBusDxe/ScsiBusDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Scsi/ScsiDiskDxe/ScsiDiskDxe.inf",
    "INF MdeModulePkg/Universal/Disk/DiskIoDxe/DiskIoDxe.inf",
    "INF MdeModulePkg/Universal/Disk/PartitionDxe/PartitionDxe.inf",
    "INF FatPkg/EnhancedFatDxe/Fat.inf",
]
dsc_sources = [device / "DXE.dsc.inc", device / "Charger.dsc.inc", root / "MUCommonCrPkg/MUCommonCr.dsc.inc", emulator_pkg / "X86Emulator.dsc.inc"]
dsc_entries = {line for path in dsc_sources for line in active_lines(path)}
mp_entry = "CranePkg/Driver/MpServicesCrDxe/MpServicesCrDxe.inf"
if mp_entry not in dsc_entries:
    raise SystemExit("DSC source manifest: native MP Services is not compiled")
if expanded_apriori.count("INF " + mp_entry) != 1:
    raise SystemExit("APRIORI.inc: expected exactly one native MP Services provider")
if any("ArmPsciMpServicesDxe" in line for line in expanded_apriori + list(fdf_entries) + list(dsc_entries)):
    raise SystemExit("HDK8450 must not select both native and stock PSCI MP Services")
if "CranePkg/Driver/RamPartitionCrDxe/RamPartitionCrDxe.inf" not in dsc_entries:
    raise SystemExit("DSC source manifest: native RAM discovery is not compiled")
buttons_entry = "CranePkg/Driver/ButtonsCrDxe/ButtonsCrDxe.inf"
if buttons_entry not in dsc_entries:
    raise SystemExit("DSC source manifest: native button driver is not compiled")
spmi_entry = "CranePkg/Driver/SpmiCrDxe/SpmiCrDxe.inf"
if spmi_entry not in dsc_entries or "INF " + spmi_entry not in fdf_entries:
    raise SystemExit("SPMI source manifest: shared button provider is not compiled and packaged")
if expanded_apriori.count("INF " + spmi_entry) != 1:
    raise SystemExit("APRIORI.inc: expected exactly one shared SPMI provider before Buttons")
charger_entries = [
    "INF CranePkg/Driver/BatteryChargerCrDxe/BatteryChargerCrDxe.inf",
    "INF CranePkg/Application/ChargerCr/ChargerCr.inf",
]
for entry in charger_entries:
    if entry not in fdf_entries or entry.removeprefix("INF ") not in dsc_entries:
        raise SystemExit(f"Charger source manifest: missing compiled/packaged entry: {entry}")
for entry in storage_entries:
    if entry not in fdf_entries:
        raise SystemExit(f"FV source manifest: storage driver is not packaged: {entry}")
    if entry.removeprefix("INF ") not in dsc_entries:
        raise SystemExit(f"DSC source manifest: storage driver is not compiled: {entry}")

typec_entries = [
    "INF CranePkg/Driver/PILCrDxe/PILCrDxe.inf",
    "INF CranePkg/Driver/UCSICrDxe/UCSICrDxe.inf",
    "INF CranePkg/Driver/UsbDpPhyCrDxe/UsbDpPhyCrDxe.inf",
    "INF CranePkg/Driver/MDSSDpCrDxe/MDSSDpCrDxe.inf",
    "INF CranePkg/Driver/Dwc3CrDxe/Dwc3CrDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/NonDiscoverablePciDeviceDxe/NonDiscoverablePciDeviceDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/XhciDxe/XhciDxe.inf",
    "INF CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Usb/UsbBusDxe/UsbBusDxe.inf",
    "INF MdeModulePkg/Bus/Usb/UsbKbDxe/UsbKbDxe.inf",
    "INF MdeModulePkg/Bus/Usb/UsbMassStorageDxe/UsbMassStorageDxe.inf",
    "INF MdeModulePkg/Bus/Usb/UsbMouseAbsolutePointerDxe/UsbMouseAbsolutePointerDxe.inf",
]
for entry in typec_entries:
    if entry not in fdf_entries or entry.removeprefix("INF ") not in dsc_entries:
        raise SystemExit(f"Type-C source manifest: native driver is not compiled and packaged: {entry}")

msc_entries = ["INF CranePkg/Application/UsbMscTest/UsbMscTest.inf"]
for entry in msc_entries:
    if entry not in fdf_entries or entry.removeprefix("INF ") not in dsc_entries:
        raise SystemExit("USB MSC Shell diagnostic must be compiled and packaged")
    if entry in expanded_apriori:
        raise SystemExit("USB MSC diagnostic is manually launched, not dispatched from APRIORI")

emulator_entries = ["INF X86EmulatorPkg/Driver/X86EmulatorDxe/X86EmulatorDxe.inf"]
if list(emulator_pkg.rglob("*.inf")) != [emulator_pkg / "Driver/X86EmulatorDxe/X86EmulatorDxe.inf"]:
    raise SystemExit("X86EmulatorPkg must contain exactly its single upstream driver INF")
for entry in emulator_entries:
    if entry.removeprefix("INF ") not in dsc_entries:
        raise SystemExit("X86EmulatorDxe is absent from the DSC")
emulator_fdf = active_lines(emulator_pkg / "X86Emulator.fdf.inc")
for entry in (
    "FILE DRIVER = E6727A5E-CBCD-44C8-B37F-78BC3A0C16C8 {",
    "SECTION DXE_DEPEX = X86EmulatorPkg/Driver/X86EmulatorDxe/AArch64/EmulatorDxe.depex",
    "SECTION PE32 = X86EmulatorPkg/Driver/X86EmulatorDxe/AArch64/EmulatorDxe.efi",
):
    if entry not in emulator_fdf:
        raise SystemExit("X86EmulatorDxe must explicitly package its AArch64 image and depex")
for name in ("WaipioCr.dsc", "WaipioCrNoSb.dsc"):
    if "!include X86EmulatorPkg/X86Emulator.dsc.inc" not in active_lines(root / "WaipioCrPkg" / name):
        raise SystemExit(f"{name}: X86EmulatorPkg is not included")
if "!include X86EmulatorPkg/X86Emulator.fdf.inc" not in active_lines(fdf):
    raise SystemExit("WaipioCr.fdf: X86EmulatorPkg is not included")
origin = json.loads((emulator_pkg / "Upstream.json").read_text())
for name, expected in origin["files"].items():
    if hashlib.sha256((emulator_pkg / "Driver/X86EmulatorDxe" / name).read_bytes()).hexdigest() != expected:
        raise SystemExit(f"X86EmulatorPkg: modified upstream file: {name}")
emulator_binary = (emulator_pkg / "Driver/X86EmulatorDxe/AArch64/EmulatorDxe.efi").read_bytes()
pe = struct.unpack_from("<I", emulator_binary, 0x3c)[0]
if emulator_binary[pe:pe+4] != b"PE\0\0" or struct.unpack_from("<H", emulator_binary, pe+4)[0] != 0xaa64:
    raise SystemExit("X86EmulatorPkg: driver is not an AArch64 PE image")


def inf_guid(inf_entry):
    """Resolve an INF path from either the platform or MU tree."""
    relative = inf_entry.removeprefix("INF ").strip()
    for source_root in (root, root.parent / "MU_BASECORE", root.parent / "Common/MU"):
        candidate = source_root / relative
        if candidate.exists():
            match = re.search(
                r"(?im)^\s*FILE_GUID\s*=\s*([0-9a-f-]{36})\s*$",
                candidate.read_text(),
            )
            if match:
                return match.group(1).lower()
            raise SystemExit(f"{candidate}: INF has no FILE_GUID")
    raise SystemExit(f"INF is not found in source trees: {relative}")


def guid_bytes(guid):
    fields = guid.split("-")
    return struct.pack(
        "<IHH", int(fields[0], 16), int(fields[1], 16), int(fields[2], 16)
    ) + bytes.fromhex(fields[3] + fields[4])


def check_built_apriori(build):
    """Verify the explicitly selected build, never an unrelated old output."""
    build = build.resolve()
    if not build.is_dir():
        raise SystemExit(f"generated FV: build directory does not exist: {build}")
    apriori_files = list(build.glob("FV/Ffs/*/*.Apri"))
    if len(apriori_files) != 1:
        raise SystemExit(
            f"generated FV: expected one APRIORI raw section, found {len(apriori_files)}"
        )
    apriori_file = apriori_files[0]
    raw = apriori_file.read_bytes()
    if len(raw) % 16:
        raise SystemExit(f"{apriori_file}: APRIORI raw section is not GUID aligned")
    generated_guids = [
        raw[offset : offset + 16] for offset in range(0, len(raw), 16)
    ]
    source_entries = [line for line in expanded_apriori if line.startswith("INF ")]
    if len(generated_guids) != len(source_entries):
        raise SystemExit(
            f"{apriori_file}: source has {len(source_entries)} entries, "
            f"build has {len(generated_guids)} GUIDs"
        )
    # Check every literal INF at its exact source position.  The crypto INF
    # uses build macros; its FFS is still checked below along with all GUIDs.
    for index, entry in enumerate(source_entries):
        if "$(" not in entry and generated_guids[index] != guid_bytes(inf_guid(entry)):
            raise SystemExit(f"{apriori_file}: wrong GUID at index {index} for {entry}")
    expected_guids = [guid_bytes(inf_guid(entry)) for entry in critical_order]
    cursor = 0
    for entry, expected in zip(critical_order, expected_guids):
        try:
            cursor = generated_guids.index(expected, cursor)
        except ValueError as error:
            raise SystemExit(
                f"{apriori_file}: missing or out-of-order APRIORI GUID for {entry}"
            ) from error
        cursor += 1

    fv_inf = build / "FV/FVMAIN.inf"
    if not fv_inf.exists():
        raise SystemExit(f"generated FV: missing {fv_inf}")
    fv_text = fv_inf.read_text().lower()
    for generated in generated_guids:
        guid = str(uuid.UUID(bytes_le=generated))
        if guid not in fv_text:
            raise SystemExit(f"{fv_inf}: APRIORI FFS ({guid}) is absent")
    for entry in critical_order + storage_entries + typec_entries + emulator_entries + charger_entries + msc_entries:
        guid = inf_guid(entry)
        if guid not in fv_text:
            raise SystemExit(f"{fv_inf}: FFS for {entry} ({guid}) is absent")
    # Inspect the FFS actually referenced by this FV, ignoring stale files from
    # an earlier packaging rule. There must be exactly one PE and one depex.
    emulator_guid = inf_guid(emulator_entries[0])
    emulator_ffs = [line.split("=", 1)[1].strip() for line in fv_inf.read_text().splitlines()
                    if "=" in line and emulator_guid in line.lower() and line.rstrip().lower().endswith(".ffs")]
    if len(emulator_ffs) != 1:
        raise SystemExit("generated FV: expected exactly one emulator FFS")
    raw_ffs = Path(emulator_ffs[0]).read_bytes()
    if raw_ffs[:16] != guid_bytes(emulator_guid) or raw_ffs[18] != 7:
        raise SystemExit("generated emulator FFS: wrong GUID or file type")
    ffs_size = int.from_bytes(raw_ffs[20:23], "little")
    sections = {}
    offset = 24
    while offset < ffs_size:
        size = int.from_bytes(raw_ffs[offset:offset+3], "little")
        if size < 4 or offset + size > ffs_size:
            raise SystemExit("generated emulator FFS: invalid section size")
        sections.setdefault(raw_ffs[offset+3], []).append(raw_ffs[offset+4:offset+size])
        offset = (offset + size + 3) & ~3
    expected_depex = (emulator_pkg / "Driver/X86EmulatorDxe/AArch64/EmulatorDxe.depex").read_bytes()
    if sections.get(0x10) != [emulator_binary] or sections.get(0x13) != [expected_depex]:
        raise SystemExit("generated emulator FFS: PE/depex differ from the single upstream AArch64 image")
    # Verify new charger images in the actual FV, not only the generated INF
    # or stale Ffs directories. Compare complete PE payloads to this build.
    fv = (build / "FV/FVMAIN.Fv").read_bytes()
    if len(fv) < 56 or fv[40:44] != b"_FVH" or int.from_bytes(fv[32:40], "little") != len(fv):
        raise SystemExit("charger FV audit: invalid FV bounds")
    offset = (int.from_bytes(fv[48:50], "little") + 7) & ~7
    expected = {
        guid_bytes(inf_guid(charger_entries[0])): (7, "BatteryChargerCrDxe"),
        guid_bytes(inf_guid(charger_entries[1])): (9, "ChargerCr"),
        guid_bytes(inf_guid(msc_entries[0])): (9, "UsbMscTest"),
    }
    seen = set()
    while offset < len(fv):
        if fv[offset:] == b"\xff" * (len(fv)-offset):
            break
        if offset+24 > len(fv):
            raise SystemExit("charger FV audit: truncated FFS")
        size = int.from_bytes(fv[offset+20:offset+23], "little")
        if size < 24 or size == 0xFFFFFF or size > len(fv)-offset:
            raise SystemExit("charger FV audit: invalid FFS size")
        guid = fv[offset:offset+16]
        if guid in expected:
            kind, name = expected[guid]
            if guid in seen or fv[offset+18] != kind:
                raise SystemExit("charger FV audit: duplicate or wrong file type")
            seen.add(guid)
            cursor, pe = offset+24, []
            while cursor < offset+size:
                length = int.from_bytes(fv[cursor:cursor+3], "little")
                if cursor+4 > offset+size or length < 4 or length > offset+size-cursor:
                    raise SystemExit("charger FV audit: invalid section")
                if fv[cursor+3] == 0x10:
                    pe.append(fv[cursor+4:cursor+length])
                cursor = (cursor+length+3) & ~3
            if pe != [(build / "AARCH64" / (name+".efi")).read_bytes()]:
                raise SystemExit("charger FV audit: PE bytes do not match build: " + name)
        offset = (offset+size+7) & ~7
    if seen != set(expected):
        raise SystemExit("charger FV audit: actual battery driver/application missing")
    print(f"HDK8450 generated FV: {build}; APRIORI order and native/storage FFS inclusion verified")
    return True


if args.build_dir is not None:
    check_built_apriori(args.build_dir)

active_fdf = [line.split("#", 1)[0].strip() for line in fdf.read_text().splitlines()]
active_fdf = [(number, line) for number, line in enumerate(active_fdf, 1) if line]
required = {
    "crane": "!include CranePkg/Crane.fdf.inc",
    "mdss": "INF CranePkg/Driver/MDSSCrDxe/MDSSCrDxe.inf",
    "lt9611": "INF CranePkg/Driver/Lt9611uxcCrDxe/lt9611.inf",
}
positions = {}
for number, line in active_fdf:
    for name, expected in required.items():
        if line == expected:
            positions[name] = number
missing = [name for name in required if name not in positions]
if missing:
    raise SystemExit(f"WaipioCr.fdf: missing active entries: {', '.join(missing)}")
if not (positions["crane"] < positions["mdss"] < positions["lt9611"]):
    raise SystemExit(
        "WaipioCr.fdf: active order must be Crane.fdf.inc, MDSSCrDxe, Lt9611Dxe"
    )

print("HDK8450 active manifests: native hardware drivers plus pinned X86 emulator; APRIORI/FV order and SD/UFS storage chains verified")
