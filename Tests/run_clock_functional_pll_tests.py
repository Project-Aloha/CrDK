#!/usr/bin/env python3
"""Actual selected SM8250/SM8350 targets: cold functional PLL lifetime, inherited preservation and retained supplier failures."""
from pathlib import Path
import argparse, hashlib, json, subprocess
pkg=Path(__file__).resolve().parents[1]; root=pkg.parents[1]
a=argparse.ArgumentParser(); a.add_argument('--output',type=Path,required=True)
a.add_argument('--before',type=Path); a.add_argument('--expect-red',action='store_true')
a.add_argument('--expect-root-red',action='store_true'); args=a.parse_args()
out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
sources=[pkg/'Tests/ClockFunctionalPllTest.c',pkg/'Library/ClockLib/fixed_pll.c',
 *[pkg/'Library/ClockExternalLib'/f for f in ('ClockExternal.c','ClockExternalPlan.c','ClockExternalLease.c')],
 pkg/'Library/MdssHandoffCrLib/Handoff.c']
if args.before:
 sources=[(args.before/s.relative_to(root)).resolve() if s.name in ('fixed_pll.c','ClockExternal.c','ClockExternalLease.c') else s for s in sources]
inputs=set(sources+[Path(__file__)]); runs=[]; failures=[]
cases=('cold','live','live-drift','live-fsm','foreign','generation','no-floor','fsm','partial-config','active-op','active-out','foreign-ref','mdp-live','lut-live','rot-live','rot-force','rot-parent','ahb-parent','lock-never','lock-stale','floor-loss','quiet-loss','fsm-loss','retry-calibrated','release-drop-0','release-drop-c','release-drop-38',*[f'drop-{o:x}' for o in (4,8,0x40,0x18,0x1c,0x20,0x10,0x2c,0)])
if args.expect_red: cases=('cold','live')
for soc in ('8250','8350'):
 for frontend in (False,True) if not args.expect_red else (False,):
  kernel=False
  mode='dxe' if frontend else 'efi'; exe=out/(soc+'-'+mode); dep=out/(exe.name+'.d')
  flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
   '-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
   '-DTEST_SOC=0x'+soc,'-I'+str(pkg/'Include')]
  if args.before: flags+=['-DTEST_BEFORE']
  if frontend:flags+=['-DTEST_FRONTEND']
  if kernel: flags+=['-D_KERNEL_MODE','-pthread','-I'+str(pkg/'Tests/KernelMocks')]
  else: flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
  compiled=sources+([pkg/'Library/PepCrLib/PepJournal.c',pkg/'Library/PepCrLib/PepClock.c',pkg/'Driver/ClockCrDxe/GpuBootAccess.c'] if frontend else [])
  cmd=flags+['-MMD','-MF',str(dep)]+list(map(str,compiled))+['-o',str(exe)]
  r=subprocess.run(cmd,capture_output=True,text=True); (out/(exe.name+'-compile.log')).write_text(r.stdout+r.stderr)
  if r.returncode: print(r.stderr); r.check_returncode()
  # Clang's link invocation dependency file describes its last translation unit;
  # collect every source dependency separately to close all included fixtures.
  for index,s in enumerate(compiled):
   d=out/(exe.name+'-'+str(index)+'.d'); dcmd=flags+['-Wno-unused-command-line-argument','-M',str(s),'-MF',str(d)]
   dr=subprocess.run(dcmd,capture_output=True,text=True); (out/(exe.name+'-'+str(index)+'-dependency.log')).write_text(dr.stdout+dr.stderr); dr.check_returncode()
   for item in d.read_text().replace('\\\n',' ').split(':',1)[1].split():
    f=Path(item)
    if f.exists() and f.is_relative_to(root):inputs.add(f)
  for case in cases+(('frontend-admission',) if frontend else ()):
   r=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=30)
   log=out/(exe.name+'-'+case+'.log');log.write_text(r.stdout+r.stderr)
   runs.append({'soc':soc,'mode':mode,'case':case,'returncode':r.returncode,'log':str(log)})
   if r.returncode:failures.append((soc,mode,case));print(soc,mode,case,'RED',r.stderr.splitlines()[0] if r.stderr else '')
   else:print(soc,mode,case,'PASS')
records={'status':'RED' if failures else 'PASS','scope':__doc__,'runs':runs,
 'inputs':{str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)},
 'outputs':{str(f.relative_to(out)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(out.iterdir()) if f.is_file()}}
(out/'artifacts.json').write_text(json.dumps(records,indent=2)+'\n')
if args.expect_red:
 assert {(soc,mode,case) for soc,mode,case in failures}=={('8350','efi','cold')},failures
 print('PASS: authenticated saved-source cold failure; inherited control passes')
else: assert not failures,failures
