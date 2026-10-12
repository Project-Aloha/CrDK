#!/usr/bin/env python3
"""Exercise real SMEM remote mutex under native and NT sanitizer mappings."""
from pathlib import Path
import subprocess
import tempfile
p=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='crane-smem-mutex-') as name:
    out=Path(name)
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-I'+str(p/'Include')]
        if kernel:
            flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/SmemMutexMocks'),
                    '-I'+str(p/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-DMDEPKG_NDEBUG',
                    '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=out/('kernel' if kernel else 'native')
        subprocess.run(flags+[str(p/'Tests/SmemMutexTest.c'),
            str(p/'Library/SmemLib/smem_mutex.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
