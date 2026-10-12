#!/usr/bin/env python3
"""Compose the actual Shell MSC application, BOT core and DWC3 function driver."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [crane / "Include", root / "MU_BASECORE/MdePkg/Include",
            root / "MU_BASECORE/MdePkg/Include/X64", root / "MU_BASECORE/MdeModulePkg/Include"]
sources = [crane / name for name in (
    "Tests/UsbMscAppTest.c", "Library/UsbMscCrLib/UsbMsc.c",
    "Library/Dwc3CrLib/Dwc3.c", "Library/Dwc3CrLib/Dwc3Power.c", "Library/ClockLib/fixed_pll.c")]
with tempfile.TemporaryDirectory(prefix="crane-usb-msc-app-") as temporary:
    for name, extra in (("debug", []), ("release", ["-DMDEPKG_NDEBUG"])):
        output = str(Path(temporary) / name)
        subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-parameter", "-ffunction-sections",
                        "-fdata-sections", "-Wl,--gc-sections", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", *extra, *["-I" + str(p) for p in includes],
                        *map(str, sources), "-o", output], check=True)
        subprocess.run([output], check=True)
