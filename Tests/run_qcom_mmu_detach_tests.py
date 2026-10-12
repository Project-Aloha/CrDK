#!/usr/bin/env python3
"""Compile the production PrePi SMMU handoff and SM8450 caller with mocked MMIO."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [
    root / "Platforms/MUCommonCrPkg/Include",
    root / "MU_BASECORE/MdePkg/Include",
    root / "MU_BASECORE/MdePkg/Include/X64",
    root / "MU_BASECORE/ArmPkg/Include",
    root / "MU_BASECORE/ArmPlatformPkg/Include",
]
with tempfile.TemporaryDirectory(prefix="crane-early-smmu-") as tmp:
    exe = Path(tmp) / "test"
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-DNO_MSABI_VA_FUNCS",
        "-Wall", "-Wextra", "-Werror",
        "-g", "-O1", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        *["-I" + str(path) for path in includes],
        str(crane / "Tests/QcomMmuDetachHelperTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
