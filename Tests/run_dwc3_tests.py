#!/usr/bin/env python3
"""Compile real DWC3 core, function and monitor against independent fixtures."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
includes = [crane / "Include", root / "MU_BASECORE/MdePkg/Include",
            root / "MU_BASECORE/MdePkg/Include/X64", root / "MU_BASECORE/MdeModulePkg/Include"]
with tempfile.TemporaryDirectory(prefix="crane-dwc3-") as temporary:
    for fixture, extra in (("Dwc3CoreTest.c", []), ("Dwc3FunctionTest.c", []), ("Dwc3RoleTest.c", []),
                           ("Dwc3RoleTest.c", ["-DCR_DWC3_NATIVE_CLOCK_TEST"]),
                           ("NonDiscoverableDmaTest.c", [])):
        output = str(Path(temporary) / (fixture.removesuffix(".c") + ("-native" if extra else "")))
        subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall",
                        "-Wextra", "-Werror", "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
                        "-Wl,--gc-sections", "-fsanitize=address,undefined",
                        "-fno-sanitize-recover=all", *["-I" + str(p) for p in includes],
                        *extra, str(crane / "Tests" / fixture), str(crane / "Library/ClockLib/fixed_pll.c"), str(crane / "Library/Dwc3CrLib/Dwc3.c"), str(crane / "Library/Dwc3CrLib/Dwc3Power.c"), "-o", output], check=True)
        subprocess.run([output], check=True)
