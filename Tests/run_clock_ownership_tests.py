#!/usr/bin/env python3
"""Execute production clock/GDSC/SM8450 cores in native and NT sanitizer builds."""
from pathlib import Path
import argparse
import subprocess
import tempfile
p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--clock-source',type=Path)
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix="crane-clock-ownership-") as name:
    out=Path(name)
    for kernel in (False, True):
        flags=["clang","-std=c11","-g","-O1","-Wall","-Wextra","-Werror",
               "-fsanitize=address,undefined","-fno-sanitize-recover=all",
               "-ffunction-sections","-fdata-sections","-Wl,--gc-sections",
               "-I"+str(p/"Include")]
        if args.clock_source:
            flags += ['-DCR_CLOCK_SOURCE_PATH="'+str(args.clock_source.resolve())+'"',
                      '-I'+str(p/'Library/ClockLib')]
        if kernel:
            flags += ["-D_KERNEL_MODE","-pthread","-I"+str(p/"Tests/KernelMocks")]
        else:
            flags += ["-fshort-wchar","-I"+str(p.parents[1]/"MU_BASECORE/MdePkg/Include"),
                      "-I"+str(p.parents[1]/"MU_BASECORE/MdePkg/Include/X64")]
        exe=out/("kernel" if kernel else "native")
        subprocess.run(flags+[str(p/"Tests/ClockOwnershipTest.c"), str(p/"Library/ClockLib/fixed_pll.c"),
            str(p/"Library/PepCrLib/PepJournal.c"),str(p/"Library/PepCrLib/PepClock.c"),
            "-o",str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
