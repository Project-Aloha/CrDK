#!/usr/bin/env python3
"""Run shared PIL production units against native and NT IO vectors."""
from pathlib import Path
import subprocess
import tempfile
import argparse
crane=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('--source',type=Path,default=crane/'Library/PILCrLib/Pil.c')
parser.add_argument('--qmp-only',action='store_true')
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='crane-pil-core-') as name:
    for kernel in (False,True):
        flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(crane/'Include')]
        if kernel:
            flags+=['-D_KERNEL_MODE','-I'+str(crane/'Tests/PilMocks'),'-I'+str(crane/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        clients=Path(name)/('clients-nt' if kernel else 'clients-native')
        subprocess.run(flags+[str(crane/'Tests/PilClientsTest.c'),
                             str(crane/'Library/PILCrLib/PilClients.c'),'-o',str(clients)],check=True)
        subprocess.run([str(clients)],check=True,timeout=30)
        for helper in (False,True):
            extra=['-DPIL_TEST_SMCCC',str(crane/'Library/PILCrLib/PilSmccc.c')] if helper else []
            if args.qmp_only:extra+=['-DPIL_TEST_QMP_ONLY']
            exe=Path(name)/(('kernel' if kernel else 'native')+('-smccc' if helper else ''))
            subprocess.run(flags+extra+[str(crane/'Tests/PilCoreTest.c'),str(args.source),
                                  str(crane/'Library/PILCrLib/PilElf.c'),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True,timeout=30)
