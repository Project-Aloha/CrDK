#!/usr/bin/env python3
"""Private external-clock prerequisites and exact SM8450 ownership recovery."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);o=a.parse_args().output.resolve();o.mkdir(parents=True,exist_ok=True)
sources=[p/'Tests/ClockExternalLeaseTest.c',p/'Library/ClockExternalLib/ClockExternal.c',p/'Library/ClockExternalLib/ClockExternalPlan.c',p/'Library/ClockExternalLib/ClockExternalLease.c',p/'Library/ClockLib/fixed_pll.c',p/'Library/PepCrLib/PepJournal.c',p/'Library/PepCrLib/PepClock.c',p/'Library/MdssDsiCrLib/MdssDsi.c',p/'Library/MdssDsiCrLib/MdssDsiClock.c',p/'Library/MdssDscCrLib/MdssDsc.c',p/'Library/MdssCrLib/Mdss.c',p/'Library/MdssCrLib/MdssHardware.c']
inputs=sources+[Path(__file__),p/'Tests/ClockExternalTest.c',p/'Tests/ClockOwnershipTest.c',p/'Library/Sm8450ClockLib/Sm8450Clock.c']+[p/'Library/ClockLib'/n for n in ('clock.c','clock_config.c','gdsc.c','clock_votes.c')]+[p/'Include/Library'/n for n in ('ClockExternalLib.h','ClockExternalPlan.h','ClockExternalLease.h','MdssDsiCrLib.h')]
frozen={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs};runs=[]
for kernel in (False,True):
 mode='nt' if kernel else 'native';exe=o/mode
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
 if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 cmd=flags+list(map(str,sources))+['-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(o/(mode+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(o/(mode+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();runs.append({'mode':mode,'command':cmd})
if frozen!={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}:raise RuntimeError('source drift')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':frozen,'runs':runs},indent=2)+'\n')
