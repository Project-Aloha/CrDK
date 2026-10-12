#!/usr/bin/env python3
"""Exercise the production FSA4480/combo/HS PHY without board MMIO."""
from pathlib import Path
import subprocess
import tempfile
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [crane / "Include", root / "Platforms/MUCommonCrPkg/Include",
            root / "MU_BASECORE/MdePkg/Include", root / "MU_BASECORE/MdePkg/Include/X64"]
with tempfile.TemporaryDirectory(prefix="crane-usbdp-") as temporary:
    output = str(Path(temporary) / "phy")
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall",
                    "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
                    "-Wl,--gc-sections", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                    str(crane / "Tests/UsbDpPhyTest.c"),
                    str(crane / "Library/UsbDpPhyCrLib/UsbDpPhy.c"),
                    str(crane / "Driver/UsbDpPhyCrDxe/Fsa4480.c"), "-o", output], check=True)
    subprocess.run([output], check=True)
