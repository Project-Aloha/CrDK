#!/usr/bin/env python3
"""Test _KERNEL_MODE portable cores using NT/WDF API-shape mocks, not a WDK."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
sources = crane / "Library/UcsiCrLib"
with tempfile.TemporaryDirectory(prefix="crane-ucsi-kernel-") as directory:
    temporary = Path(directory)
    for name in ["Glink", "PmicGlink", "Qrtr", "ChargerLog", "Ipcc", "oskinterrupt_wdf"]:
        (temporary / (name + ".tmh")).write_text("/* WPP shape mock. */\n")
    flags = ["clang", "-std=c11", "-D_KERNEL_MODE", "-g", "-O1", "-pthread",
             "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
             "-fno-sanitize-recover=all", "-I" + str(crane / "Tests/KernelMocks"),
             "-I" + str(temporary), "-I" + str(crane / "Include")]
    # Compile every exported core without any UEFI headers or libraries.
    for name in ["Glink", "PmicGlink", "Qrtr", "ChargerLog", "Ipcc"]:
        subprocess.run([*flags, "-c", str(sources / (name + ".c")),
                        "-o", str(temporary / (name + ".o"))], check=True)
    executable = temporary / "wire"
    subprocess.run([*flags, str(crane / "Tests/UsbTypeCTransportTest.c"),
                    *[str(temporary / (name + ".o")) for name in ["Glink", "PmicGlink", "Qrtr"]],
                    "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
    executable = temporary / "interrupt"
    subprocess.run([*flags, str(crane / "Tests/OskalWdfInterruptTest.c"),
                    str(temporary / "Ipcc.o"), "-o", str(executable)], check=True)
    subprocess.run([str(executable)], check=True, timeout=30)
