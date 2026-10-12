#!/usr/bin/env python3
"""Shared ClockExternal admission against public layout fixtures, native/NT."""
from pathlib import Path
import argparse, subprocess, tempfile
p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--baseline',action='store_true')
a=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='cr-clock-external-soc-') as directory:
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
        if a.baseline:flags+=['-DCR_CLOCK_SOC_API_BASELINE']
        if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
        else:flags+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
                     '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=Path(directory)/('nt' if kernel else 'native')
        sources=[p/'Tests/ClockExternalSocTest.c',p/'Library/ClockExternalLib/ClockExternal.c',
                 p/'Library/ClockExternalLib/ClockExternalPlan.c',
                 p/'Library/ClockExternalLib/ClockExternalLease.c',p/'Library/ClockLib/fixed_pll.c',
                 p/'Library/PepCrLib/PepJournal.c',p/'Library/PepCrLib/PepClock.c',
                 p/'Library/MdssHandoffCrLib/Handoff.c']
        subprocess.run(flags+[str(s) for s in sources]+['-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
