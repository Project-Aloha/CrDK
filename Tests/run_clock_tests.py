#!/usr/bin/env python3
"""Exercise portable clock logic and the native ClockDxe adapter."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
parser.add_argument("--cc", default="clang")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="crane-clock-test-") as tmp:
    common = [
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
    ]
    for name, source in (
        ("clock-lib", "ClockLibTest.c"),
        ("clock-adapter", "ClockCrDxeTest.c"),
    ):
        exe = Path(tmp) / name
        subprocess.run(common + [str(crane / "Tests" / source),
                                 "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
