#!/usr/bin/env python3
"""Actual ClockLib empty controller handling, native and NT."""
from pathlib import Path
import argparse,subprocess,tempfile
p=Path(__file__).resolve().parents[1]
a=argparse.ArgumentParser();a.add_argument('--clock-source',type=Path);args=a.parse_args()
with tempfile.TemporaryDirectory(prefix='clock-holes-') as directory:
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
        if args.clock_source:flags+=['-DCR_CLOCK_SOURCE_PATH="'+str(args.clock_source.resolve())+'"']
        if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
        else:flags+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
                     '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=Path(directory)/('nt' if kernel else 'native')
        subprocess.run(flags+[str(p/'Tests/ClockControllerHolesTest.c'),str(p/'Library/ClockLib/fixed_pll.c'),
            '-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
