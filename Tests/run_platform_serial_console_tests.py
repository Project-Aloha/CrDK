#!/usr/bin/env python3
"""Exercise production GENI, SerialDxe and platform console selection together."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [
    root / "MU_BASECORE/MdePkg/Include",
    root / "MU_BASECORE/MdePkg/Include/X64",
    root / "MU_BASECORE/MdeModulePkg/Include",
    root / "Platforms/MUCommonCrPkg/Include",
    root / "Common/MU/PcBdsPkg/Include",
    root / "Common/MU/MsCorePkg/Include",
]
with tempfile.TemporaryDirectory(prefix="crane-serial-console-") as tmp:
    executable = Path(tmp) / "serial-console-test"
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-Wno-missing-field-initializers", "-DMDEPKG_NDEBUG",
        "-fsanitize=address,undefined", "-fno-sanitize=alignment", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        *["-I" + str(path) for path in includes],
        str(crane / "Tests/PlatformSerialConsoleTest.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
