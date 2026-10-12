#!/usr/bin/env python3
"""Production ARC/XO tables and owner bank, native/NT with sanitizers."""
from pathlib import Path
import argparse
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path)
args=p.parse_args()
source=args.source.resolve() if args.source else crane/'Library/RpmhArcVoteLib/RpmhArcVote.c'
with tempfile.TemporaryDirectory(prefix='cr-rpmh-arc-') as directory:
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(crane/'Include')]
        if kernel:
            flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=Path(directory)/('kernel' if kernel else 'native')
        subprocess.run(flags+[str(source),str(crane/'Tests/RpmhArcOwnershipTest.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
