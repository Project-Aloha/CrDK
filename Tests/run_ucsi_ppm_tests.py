#!/usr/bin/env python3
"""Compile actual OS-owned UCSI transport under native and NT API shapes."""
from pathlib import Path
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix="cr-ucsi-ppm-") as directory:
    for kernel in (False,True):
        flags=["clang","-std=c11","-Wall","-Wextra","-Werror","-g","-O1",
               "-fsanitize=address,undefined","-fno-sanitize-recover=all"]
        flags += ["-D_KERNEL_MODE"] if kernel else []
        flags += ["-I"+str(root/"Include")]
        flags += ["-I"+str(root/"Tests/KernelMocks")] if kernel else ["-fshort-wchar", "-I"+str(root.parents[1]/"MU_BASECORE/MdePkg/Include"), "-I"+str(root.parents[1]/"MU_BASECORE/MdePkg/Include/X64")]
        exe=Path(directory)/("nt" if kernel else "native")
        subprocess.run([*flags,str(root/"Tests/UcsiPpmTest.c"),str(root/"Library/UcsiCrLib/Ppm.c"),"-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
