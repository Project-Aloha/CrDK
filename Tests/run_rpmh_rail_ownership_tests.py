#!/usr/bin/env python3
"""Production supply-graph leases with native and NT sanitizers."""
from pathlib import Path
import argparse
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--votes-source',type=Path,help='Replay ownership vectors against a saved vote-bank source.')
args=parser.parse_args()
votes_source=args.votes_source.resolve() if args.votes_source else crane/'Library/RpmhLib/rpmh_rail_votes.c'
with tempfile.TemporaryDirectory(prefix='cr-rpmh-rail-') as name:
    out=Path(name)
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all',
               '-I'+str(crane/'Include')]
        if kernel:
            flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')]
        else:
            flags+=['-fshort-wchar','-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include'),
                    '-I'+str(crane.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=out/('kernel' if kernel else 'native')
        subprocess.run(flags+[str(crane/'Library/RpmhLib/rpmh_rail.c'),
            str(votes_source),
            str(crane/'Library/PepCrLib/PepRail.c'),str(crane/'Library/PepCrLib/PepJournal.c'),
            str(crane/'Tests/RpmhRailOwnershipTest.c'),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)
