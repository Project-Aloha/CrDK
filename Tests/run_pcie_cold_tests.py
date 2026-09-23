#!/usr/bin/env python3
"""Run real PCIe cold initialization and SM8450 data with resource/MMIO mocks."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
edk2 = crane.parents[1] / "MU_BASECORE"
with tempfile.TemporaryDirectory(prefix="crane-pcie-cold-") as tmp:
    exe = Path(tmp) / "pcie-cold"
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-ffunction-sections",
        "-fdata-sections", "-Wl,--gc-sections",
        "-I" + str(crane / "Include"),
        "-I" + str(edk2 / "MdePkg/Include"),
        "-I" + str(edk2 / "MdePkg/Include/X64"),
        str(crane / "Tests/PcieColdInitTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
