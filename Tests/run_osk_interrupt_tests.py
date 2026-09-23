#!/usr/bin/env python3
"""Exercise the real UEFI interrupt adapter against failure-injecting mocks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="crane-osk-irq-test-") as tmp:
    exe = Path(tmp) / "osk-irq-test"
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        "-I" + str(args.edk2 / "EmbeddedPkg/Include"),
        str(crane / "Tests/OskalInterruptTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
