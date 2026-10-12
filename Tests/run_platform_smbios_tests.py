#!/usr/bin/env python3
"""Run production SMBIOS publication without optional OEM protocols."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [
    root / "MU_BASECORE/MdePkg/Include",
    root / "MU_BASECORE/MdePkg/Include/X64",
    root / "Platforms/MUCommonCrPkg/Include",
    root / "Common/MU/PcBdsPkg/Include",
]
# Hash2 is consumed only by the optional UUID helper, never a dispatch gate.
inf = root / "Platforms/MUCommonCrPkg/Driver/SmBiosTableDxe/SmBiosTableDxe.inf"
depex = inf.read_text().split("[Depex]", 1)[1].split("[", 1)[0].strip()
assert depex == "gEfiSmbiosProtocolGuid", depex

with tempfile.TemporaryDirectory(prefix="crane-smbios-") as tmp:
    executable = Path(tmp) / "smbios-test"
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers",
        "-Wno-missing-braces", "-Wno-bitfield-constant-conversion",
        "-Wno-excess-initializers", "-DMDEPKG_NDEBUG",
        "-fsanitize=address,undefined", "-fno-sanitize=alignment", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        *["-I" + str(path) for path in includes],
        str(crane / "Tests/PlatformSmbiosTest.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
