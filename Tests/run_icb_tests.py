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
    for kernel in (False, True):
        flags = [args.cc, "-std=c11", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                 "-I" + str(crane / "Include"), "-I" + str(Path(tmp))]
        if kernel:
            flags += ["-D_KERNEL_MODE", "-pthread", "-I" + str(crane / "Tests/KernelMocks")]
            (Path(tmp) / "icb.tmh").write_text("/* Host WPP substitute. */\n")
        else:
            flags += ["-fshort-wchar", "-DMDEPKG_NDEBUG", *includes]
        subprocess.run(flags + [
            str(crane / "Library/ICBLib/icb.c"),
            str(crane / "Library/ICBLib/icb_votes.c"),
            str(crane / "Library/PepCrLib/PepBandwidth.c"),
            str(crane / "Library/PepCrLib/PepJournal.c"),
            str(crane / "Target/Sm8450/CrInterconnectTarget.c"),
            str(crane / "Library/Sm8450InterconnectLib/Sm8450Interconnect.c"),
            str(crane / "Tests/ICBLibTest.c"), "-o", str(exe),
        ], check=True)
        subprocess.run([str(exe)], check=True, timeout=30)
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1", "-include", "Uefi.h",
        "-Wall", "-Wextra", "-Werror", "-DMDEPKG_NDEBUG",
        "-fsanitize=address,undefined", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", *includes,
        str(crane / "Library/ICBLib/icb.c"),
        str(crane / "Target/Sm8450/CrInterconnectTarget.c"),
        str(crane / "Library/Sm8450InterconnectLib/Sm8450Interconnect.c"),
        str(crane / "Tests/ICBCrDxeTest.c"),
        "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True)
