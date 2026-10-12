#!/usr/bin/env python3
"""Test the production GENI SerialPortLib receive path with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-geni-serial-") as tmp:
    executable = Path(tmp) / "geni-serial-test"
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I" + str(root / "MU_BASECORE/MdePkg/Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include/X64"),
        str(crane / "Tests/GeniSerialConInTest.c"), "-o", str(executable),
    ], check=True)
    subprocess.run([str(executable)], check=True)
