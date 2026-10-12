#!/usr/bin/env python3
"""Test real PDC core under native and NT sanitizer mappings."""
import argparse
from pathlib import Path
import subprocess
import tempfile
p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,default=p/'Library/PdcLib/pdc.c')
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='crane-pdc-bounds-') as name:
    out=Path(name)
    (out/'pdc.tmh').write_text('/* Host WPP substitute. */\n')
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-I'+str(p/'Include'),'-I'+str(p/'Library/PdcLib'),'-I'+str(out)]
        if kernel:
            flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-DMDEPKG_NDEBUG',
                    '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=out/('kernel' if kernel else 'native')
        subprocess.run(flags+[str(p/'Tests/PdcBoundsTest.c'),str(args.source),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
