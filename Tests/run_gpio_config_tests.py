#!/usr/bin/env python3
"""Check portable configuration using the real Windows SM8450 ACPI tables."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
from asl_literal import named_package, c_fixture

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--acpi", type=Path, default=Path.home()/"CrWDDK/AcpiCrDsdt/sm8450/crane")
args = parser.parse_args()
crane = Path(__file__).resolve().parents[1]
workspace = crane.parents[1]
generated = args.acpi/"generated/gpio_generated.asl"
gpio = args.acpi/"gpio.asl"
tables = {"Registers":named_package(generated.read_text(),"RCFG"),
          "Tiles":named_package(generated.read_text(),"TLMT"),
          "Pins":named_package(generated.read_text(),"GPLT"),
          "Ranges":named_package(gpio.read_text(),"PDCR"),
          "Reserved":named_package(gpio.read_text(),"RSGR"),
          "Direct":named_package(gpio.read_text(),"DPIN")}
with tempfile.TemporaryDirectory(prefix="crane-gpio-config-") as name:
    tmp = Path(name)
    (tmp/"GpioConfigFixture.h").write_text(c_fixture(tables))
    (tmp/"pdc.tmh").write_text("/* Host WPP substitute */\n")
    for kernel in (False, True):
        flags = ["clang","-std=c11","-g","-O1","-Wall","-Wextra","-Werror",
                 "-fsanitize=address,undefined","-fno-sanitize-recover=all",
                 "-I"+str(tmp),"-I"+str(crane/"Include"),
                 "-I"+str(crane/"Library/PdcLib")]
        if kernel: flags += ["-D_KERNEL_MODE","-pthread","-I"+str(crane/"Tests/KernelMocks")]
        else: flags += ["-fshort-wchar","-DMDEPKG_NDEBUG","-I"+str(workspace/"MU_BASECORE/MdePkg/Include"),
                        "-I"+str(workspace/"MU_BASECORE/MdePkg/Include/X64")]
        binary=tmp/("kernel" if kernel else "native")
        subprocess.run([*flags,str(crane/"Tests/GpioConfigTest.c"),
                        str(crane/"Library/GpioConfigLib/GpioConfig.c"),
                        str(crane/"Library/PdcLib/pdc.c"),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=30)
for path in (generated,gpio): print(path.name,hashlib.sha256(path.read_bytes()).hexdigest())
