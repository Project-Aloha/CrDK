#!/usr/bin/env python3
"""Exercise actual MDP map code; other resource functions are linker-discarded."""
from pathlib import Path
import subprocess
import tempfile
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-mdp-map-") as tmp:
    exe = str(Path(tmp) / "test")
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-O1", "-g", "-Wall", "-Wextra",
        "-Werror", "-Wno-unused-parameter", "-DMDEPKG_NDEBUG",
        "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
        "-fsanitize=address,undefined",
        "-I" + str(crane / "Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include"),
        "-I" + str(root / "MU_BASECORE/MdePkg/Include/X64"),
        "-I" + str(root / "MU_BASECORE/MdeModulePkg/Include"),
        str(crane / "Tests/MdpResourceMapTest.c"), "-o", exe,
    ], check=True)
    subprocess.run([exe], check=True)
