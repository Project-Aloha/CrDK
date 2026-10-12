#!/usr/bin/env python3
"""Exercise the production native clock frontend and core together under sanitizers."""
import argparse
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--cc", default="clang")
parser.add_argument("--edk2", type=Path, default=crane.parents[1] / "MU_BASECORE")
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix="crane-native-clock-ownership-") as directory:
    exe = Path(directory) / "clock-native-ownership"
    subprocess.run([
        args.cc, "-std=c11", "-fshort-wchar", "-g", "-O1",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-I" + str(crane / "Include"),
        "-I" + str(args.edk2 / "MdePkg/Include"),
        "-I" + str(args.edk2 / "MdePkg/Include/X64"),
        str(crane / "Tests/ClockCrNativeOwnershipTest.c"), str(crane / "Library/ClockLib/fixed_pll.c"),
        str(crane / "Driver/ClockCrDxe/GpuBootAccess.c"),
        *[str(crane / "Library/ClockExternalLib" / name) for name in
          ("ClockExternal.c", "ClockExternalPlan.c", "ClockExternalLease.c")],
        "-o", str(exe),
    ], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
