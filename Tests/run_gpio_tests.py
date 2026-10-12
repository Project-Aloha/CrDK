#!/usr/bin/env python3
"""Compile actual GPIO library/SM8450 target and validate Linux MMIO addresses."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-gpio-") as tmp:
    executable = str(Path(tmp) / "gpio-test")
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
        "-Werror", "-include", "Uefi.h", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-I" + str(crane / "Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include/X64"),
        str(crane / "Tests/GpioRegisterTest.c"),
        str(crane / "Library/GpioLib/gpio.c"),
        str(crane / "Target/Sm8450/CrGpioTarget.c"),
        "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
