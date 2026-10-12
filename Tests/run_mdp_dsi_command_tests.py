#!/usr/bin/env python3
"""Exercise actual command transport using MMIO and DMA mocks, ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-dsi-command-") as tmp:
    exe = str(Path(tmp) / "test")
    includes = [crane / "Include", root / "MU_BASECORE/MdePkg/Include",
                root / "MU_BASECORE/MdePkg/Include/X64"]
    subprocess.run(["clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined",
                    *["-I" + str(p) for p in includes],
                    str(crane / "Tests/MdpDsiCommandTest.c"), "-o", exe], check=True)
    subprocess.run([exe], check=True)
