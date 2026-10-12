#!/usr/bin/env python3
"""Native/NT actual journal with Kodiak policy and partial-write vote model."""
from pathlib import Path
import subprocess
import sys
import tempfile

crane=Path(__file__).resolve().parents[1]
workspace=crane.parents[1]
with tempfile.TemporaryDirectory(prefix='crane-pep-journal-') as name:
    tmp=Path(name)
    subprocess.run([sys.executable,str(crane/'Tests/run_pep_tests.py'),
                    '--emit-fixture',str(tmp/'PepFixture.h')],check=True)
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-Wno-unused-const-variable','-fsanitize=address,undefined',
               '-fno-sanitize-recover=all','-I'+str(tmp),'-I'+str(crane/'Include')]
        if kernel:
            flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-I'+str(workspace/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(workspace/'MU_BASECORE/MdePkg/Include/X64')]
        exe=tmp/('kernel' if kernel else 'native')
        subprocess.run([*flags,str(crane/'Tests/PepJournalTest.c'),
                        str(crane/'Library/PepCrLib/Pep.c'),
                        str(crane/'Library/PepCrLib/PepJournal.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
