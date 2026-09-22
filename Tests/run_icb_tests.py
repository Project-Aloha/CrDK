#!/usr/bin/env python3
"""Compile the real ICB voter and SM8450 target against host mocks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=root / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="crane-interconnect-test-") as tmp:
    exe = Path(tmp) / "icb-test"
    includes = [
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
    ]
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1", "-include", "Uefi.h",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        *includes,
        str(crane / "Library/ICBLib/icb.c"),
        str(root / "Silicon/QC/Sm8450/QcomPkg/Library/CrTargetLib/CrInterconnectTarget.c"),
        str(crane / "Tests/ICBLibTest.c"),
        "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
