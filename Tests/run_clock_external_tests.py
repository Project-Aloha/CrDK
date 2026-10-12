#!/usr/bin/env python3
"""Actual ClockLib/SM8450/external group, native and NT under sanitizers."""
from pathlib import Path
import subprocess,tempfile
p=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cr-clock-external-') as name:
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
        if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
        else:flags+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),'-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=Path(name)/('nt' if kernel else 'native')
        subprocess.run(flags+[str(p/'Tests/ClockExternalTest.c'),str(p/'Library/ClockExternalLib/ClockExternal.c'),str(p/'Library/ClockExternalLib/ClockExternalPlan.c'),str(p/'Library/ClockLib/fixed_pll.c'),str(p/'Library/PepCrLib/PepJournal.c'),str(p/'Library/PepCrLib/PepClock.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
