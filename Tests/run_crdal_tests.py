#!/usr/bin/env python3
"""Exercise the real CrDAL registry ABI using EDK2 headers and sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="crane-crdal-test-") as tmp:
    common = [
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
    ]
    cases = [
        ("registry", "Library/CrDalRegistryLib/CrDalRegistryLib.c",
         "Tests/CrDalRegistryLibTest.c"),
        ("client", "Library/CrDalLib/CrDalLib.c", "Tests/CrDalLibTest.c"),
    ]
    for name, source, test in cases:
        exe = Path(tmp) / ("crdal-" + name + "-test")
        subprocess.run(common + [str(crane / source), str(crane / test),
                                "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
