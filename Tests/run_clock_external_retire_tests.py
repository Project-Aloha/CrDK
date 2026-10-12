#!/usr/bin/env python3
"""Real shared ClockLib DSI retirement composition, native and NT."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);args=a.parse_args()
o=args.output.resolve();o.mkdir(parents=True,exist_ok=True)
sources=[p/'Tests/ClockExternalRetireTest.c',p/'Library/ClockExternalLib/ClockExternal.c',
 p/'Library/ClockExternalLib/ClockExternalPlan.c',p/'Library/ClockExternalLib/ClockExternalLease.c',
 p/'Library/ClockLib/fixed_pll.c',p/'Library/PepCrLib/PepJournal.c',p/'Library/PepCrLib/PepClock.c',
 p/'Library/MdssHandoffCrLib/Handoff.c']
records=[]
for kernel in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
  '-fsanitize=address,undefined','-fno-sanitize-recover=all',
  '-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
 if kernel:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 name='nt' if kernel else 'native';exe=o/name;cmd=flags+list(map(str,sources))+['-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True);(o/(name+'-compile.log')).write_text(r.stdout+r.stderr);print(r.stderr,end='');r.check_returncode()
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60);(o/(name+'-run.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
 records.append({'command':cmd,'sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'executions':records,
 'inputs':{str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources+[Path(__file__)]}},indent=2)+'\n')
