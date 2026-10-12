#!/usr/bin/env python3
"""Actual original LPXC + portable parser/journal on native and NT sanitizer builds."""
from pathlib import Path
import subprocess
import sys
import tempfile
crane=Path(__file__).resolve().parents[1]
workspace=crane.parents[1]
with tempfile.TemporaryDirectory(prefix='cr-pep-original-sequence-') as temporary:
    out=Path(temporary)
    subprocess.run([sys.executable,str(crane/'Tests/run_pep_tests.py'),'--emit-fixture',str(out/'PepFixture.h')],check=True)
    for kernel in (False,True):
        flags=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-const-variable',
          '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(out),'-I'+str(crane/'Include')]
        if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')]
        else:flags+=['-fshort-wchar','-I'+str(workspace/'MU_BASECORE/MdePkg/Include'),'-I'+str(workspace/'MU_BASECORE/MdePkg/Include/X64')]
        exe=out/('kernel' if kernel else 'native')
        subprocess.run([*flags,str(crane/'Tests/PepOriginalSequenceTest.c'),str(crane/'Library/PepCrLib/Pep.c'),
          str(crane/'Library/PepCrLib/PepJournal.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
