#!/usr/bin/env python3
"""Exercise actual SPMI version cores in native and NT sanitizer builds."""
from pathlib import Path
import argparse
import subprocess
import tempfile

p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--library',type=Path,default=p/'Library/SpmiLib')
parser.add_argument('--bounds-only',action='store_true')
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='crane-spmi-bounds-') as name:
    out=Path(name)
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-I'+str(p/'Include'),'-I'+str(out)]
        if args.bounds_only:
            flags+=['-DSPMI_BOUNDS_ONLY']
        if kernel:
            flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-DMDEPKG_NDEBUG',
                    '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        sources=sorted(args.library.glob('*.c'))
        for source in sources:
            (out/(source.stem+'.tmh')).write_text('/* Host WPP substitute. */\n')
        exe=out/('kernel' if kernel else 'native')
        subprocess.run(flags+[str(p/'Tests/SpmiBoundsTest.c'),*map(str,sources),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
