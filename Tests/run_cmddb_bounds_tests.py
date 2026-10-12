#!/usr/bin/env python3
"""Validate actual CmdDB implementation against malformed/truncated regions."""
from pathlib import Path
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1];root=crane.parents[1]
with tempfile.TemporaryDirectory(prefix="crane-cmddb-") as name:
    tmp=Path(name)
    (tmp/"cmddb.tmh").write_text("/* WPP test substitute. */\n")
    for kernel in (False,True):
        binary=tmp/("kernel" if kernel else "native")
        flags=["clang","-std=c11","-g","-O1","-Wall","-Wextra","-Werror",
               "-fsanitize=address,undefined","-fno-sanitize-recover=all",
               "-I"+str(crane/"Include"),"-I"+str(tmp)]
        if kernel:flags += ["-D_KERNEL_MODE","-pthread","-I"+str(crane/"Tests/KernelMocks")]
        else:flags += ["-fshort-wchar","-include","Uefi.h",
                       "-I"+str(root/"MU_BASECORE/MdePkg/Include"),
                       "-I"+str(root/"MU_BASECORE/MdePkg/Include/X64")]
        subprocess.run([*flags,str(crane/"Tests/CmdDbBoundsTest.c"),
                        str(crane/"Library/CmdDBLib/cmddb.c"),"-o",str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=30)
