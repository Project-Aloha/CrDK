#!/usr/bin/env python3
"""Exercise actual DMA pixel setup and scanout stop with ordered MMIO mocks."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-mdp-stop-") as tmp:
    executable = str(Path(tmp) / "mdp-stop-test")
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include/X64"),
        str(crane / "Tests/MdpScanoutTest.c"),
        str(crane / "Driver/MDSSCrDxe/MdpScanout.c"),
        str(crane / "Library/MdssCrLib/Mdss.c"),
        "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
