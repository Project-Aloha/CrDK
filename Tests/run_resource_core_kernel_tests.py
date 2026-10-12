#!/usr/bin/env python3
"""Run resource-core kernel behavior under NT API mocks and sanitizers."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="crane-resource-kernel-") as name:
    tmp = Path(name)
    (tmp / "pdc.tmh").write_text("/* WPP shape substitute. */\n")
    executable = tmp / "resource-core"
    subprocess.run([
        "clang", "-std=c11", "-D_KERNEL_MODE", "-g", "-O1", "-pthread",
        "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-I" + str(crane / "Tests/KernelMocks"),
        "-I" + str(tmp), "-I" + str(crane / "Include"),
        str(crane / "Tests/ResourceCoreKernelTest.c"), str(crane / "Library/ClockLib/fixed_pll.c"), "-o", str(executable)
    ], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
