#!/usr/bin/env python3
"""Compile the real SMMU core against EDK2 headers and mocked MMIO."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="crane-smmu-test-") as tmp:
    exe = Path(tmp) / "smmu-test"
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        str(crane / "Library/SmmuLib/smmu.c"),
        str(crane / "Tests/SmmuLibTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-DMDEPKG_NDEBUG",
        "-fsanitize=address,undefined", "-ffunction-sections",
        "-fdata-sections", "-Wl,--gc-sections",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        "-I" + str(args.edk2 / "MdeModulePkg/Include"),
        str(crane / "Tests/SmmuCrDxeTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        str(crane / "Tests/OskalSyncTest.c"), "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
