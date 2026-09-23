#!/usr/bin/env python3
"""Compile the real RPMh voter against a deterministic host MMIO model."""

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

with tempfile.TemporaryDirectory(prefix="crane-rpmh-test-") as tmp:
    exe = Path(tmp) / "rpmh-test"
    subprocess.run([
        args.cc,
        "-std=c11",
        "-fshort-wchar",
        "-g",
        "-O1",
        "-include",
        "Uefi.h",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-fsanitize=address,undefined",
        "-I" + str(crane / "Tests/Mocks"),
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        str(crane / "Library/RpmhLib/rpmh.c"),
        str(crane / "Tests/RpmhLibTest.c"),
        "-o",
        str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
    subprocess.run([
        args.cc,
        "-std=c11",
        "-fshort-wchar",
        "-g",
        "-O1",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-DMDEPKG_NDEBUG",
        "-fsanitize=address,undefined",
        "-ffunction-sections",
        "-fdata-sections",
        "-Wl,--gc-sections",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        str(crane / "Tests/RpmhCrDxeTest.c"),
        "-o",
        str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
