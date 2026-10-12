#!/usr/bin/env python3
"""Link the UEFI SMEM adapter with the real Crane reader against a mapped fixture."""
from pathlib import Path
import subprocess
import tempfile

crane = Path(__file__).resolve().parents[1]
root = crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-smem-sec-") as tmp:
    executable = str(Path(tmp) / "smem-sec-test")
    includes = [crane / "Include", root / "Platforms/MUCommonCrPkg/Include",
                root / "MU_BASECORE/MdePkg/Include", root / "MU_BASECORE/MdePkg/Include/X64",
                root / "MU_BASECORE/ArmPkg/Include", root / "MU_BASECORE/ArmPlatformPkg/Include"]
    subprocess.run([
        "clang", "-std=c11", "-fshort-wchar", "-g", "-O1", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        *["-I" + str(p) for p in includes],
        str(crane / "Tests/SmemSecAdapterTest.c"),
        str(crane / "Library/SmemLib/smem.c"),
        str(root / "Platforms/MUCommonCrPkg/Library/SmemSecLib/smem.c"),
        "-o", executable,
    ], check=True)
    subprocess.run([executable], check=True)
