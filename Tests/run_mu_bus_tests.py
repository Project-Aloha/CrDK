#!/usr/bin/env python3
"""Test the real MU I2C/SPI adapter code with EDK2 headers and sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="crane-mu-bus-test-") as tmp:
    common = [
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
    ]
    for source in ["I2cCrTest.c", "SpiCrTest.c"]:
        exe = Path(tmp) / Path(source).stem
        subprocess.run(common + [str(crane / "Tests" / source), "-o", str(exe)],
                       check=True)
        subprocess.run([str(exe)], check=True)
