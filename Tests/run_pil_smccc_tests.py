#!/usr/bin/env python3
"""Native/NT transport qualification; host SMC reply model, no privileged call."""
from pathlib import Path
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='cr-pil-smccc-') as directory:
    for kernel in (False,True):
        flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(crane/'Include')]
        if kernel:flags+=['-D_KERNEL_MODE','-I'+str(crane/'Tests/KernelMocks')]
        else:flags+=['-fshort-wchar','-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=Path(directory)/('kernel' if kernel else 'native')
        subprocess.run(flags+[str(crane/'Tests/PilSmcccTest.c'),str(crane/'Library/PILCrLib/PilSmccc.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
