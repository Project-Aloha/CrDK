#!/usr/bin/env python3
"""Run native FIFO/GPI behavior against fake hardware with ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
edk2 = crane.parents[1] / "MU_BASECORE"
with tempfile.TemporaryDirectory(prefix="crane-geni-test-") as tmp:
    common = ["clang", "-std=c11", "-fshort-wchar", "-g", "-O1",
              "-Wall", "-Wextra", "-Werror", "-DCRANE_HOST_TEST", "-DCRANE_MMIO_TEST",
              "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
              "-ffunction-sections", "-fdata-sections",
              "-Wl,--gc-sections", "-I" + str(crane / "Include"),
              "-I" + str(edk2 / "MdePkg/Include"),
              "-I" + str(edk2 / "MdePkg/Include/X64"),
              "-I" + str(edk2 / "MdeModulePkg/Include")]
    for source in ["GpiTransportTest.c", "I2cFifoTransportTest.c"]:
        exe = Path(tmp) / Path(source).stem
        subprocess.run(common + [str(crane / "Tests" / source), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
