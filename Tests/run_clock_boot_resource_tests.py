#!/usr/bin/env python3
"""Actual selected SM8250/SM8350 targets: qualified functional withdrawal and GDSC/AHB sequencing."""
from pathlib import Path
import argparse, hashlib, json, subprocess
pkg=Path(__file__).resolve().parents[1]; root=pkg.parents[1]
a=argparse.ArgumentParser(); a.add_argument('--output',type=Path,required=True)
a.add_argument('--before',type=Path); a.add_argument('--expect-red',action='store_true')
a.add_argument('--expect-root-red',action='store_true'); args=a.parse_args()
out=args.output.resolve(); out.mkdir(parents=True,exist_ok=True)
sources=[pkg/'Tests/ClockBootResourceTest.c',pkg/'Library/ClockLib/fixed_pll.c',
 *[pkg/'Library/ClockExternalLib'/f for f in ('ClockExternal.c','ClockExternalPlan.c','ClockExternalLease.c')],
 pkg/'Library/MdssHandoffCrLib/Handoff.c']
if args.before:
 sources=[(args.before/s.relative_to(root)).resolve() if 'ClockExternalLib' in str(s) else s for s in sources]
inputs=set(sources+[Path(__file__)]); runs=[]; failures=[]
cases=('voted-alive','local-loss','ordinary-alive','rcg-pending','rcg-force-loss',
 'root-active-0','root-active-1','root-active-2','root-active-3','roots-active','vsync-cmd-zero','descriptor-drift',
 'collapsed','release-order','domain-failure','ahb-failure','child-release-failure','domain-release-failure','shared-votes')
if args.expect_red:
 cases=tuple(c for c in cases if c not in ('rcg-force-loss','root-active-0','root-active-1',
  'root-active-2','root-active-3','roots-active','vsync-cmd-zero','descriptor-drift'))
for soc in ('8250','8350'):
 for kernel in (False,):
  mode='nt' if kernel else 'efi'; exe=out/(soc+'-'+mode); dep=out/(exe.name+'.d')
  flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
   '-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
   '-DTEST_SOC=0x'+soc,'-I'+str(pkg/'Include')]
  if kernel: flags+=['-D_KERNEL_MODE','-pthread','-I'+str(pkg/'Tests/KernelMocks')]
  else: flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
  cmd=flags+['-MMD','-MF',str(dep)]+list(map(str,sources))+['-o',str(exe)]
  r=subprocess.run(cmd,capture_output=True,text=True); (out/(exe.name+'-compile.log')).write_text(r.stdout+r.stderr)
  if r.returncode: print(r.stderr); r.check_returncode()
  # Clang's link invocation dependency file describes its last translation unit;
  # collect every source dependency separately to close all included fixtures.
  for index,s in enumerate(sources):
   d=out/(exe.name+'-'+str(index)+'.d'); dcmd=flags+['-Wno-unused-command-line-argument','-M',str(s),'-MF',str(d)]
   dr=subprocess.run(dcmd,capture_output=True,text=True); (out/(exe.name+'-'+str(index)+'-dependency.log')).write_text(dr.stdout+dr.stderr); dr.check_returncode()
   for item in d.read_text().replace('\\\n',' ').split(':',1)[1].split():
    f=Path(item)
    if f.exists() and f.is_relative_to(root):inputs.add(f)
  for case in cases:
   r=subprocess.run([str(exe),case],capture_output=True,text=True,timeout=30)
   log=out/(exe.name+'-'+case+'.log');log.write_text(r.stdout+r.stderr)
   runs.append({'soc':soc,'mode':mode,'case':case,'returncode':r.returncode,'log':str(log)})
   if r.returncode:failures.append((soc,mode,case));print(soc,mode,case,'RED',r.stderr.splitlines()[0] if r.stderr else '')
   else:print(soc,mode,case,'PASS')
records={'status':'RED' if failures else 'PASS','scope':__doc__,'runs':runs,
 'inputs':{str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(inputs)},
 'outputs':{str(f.relative_to(out)):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(out.iterdir()) if f.is_file()}}
(out/'artifacts.json').write_text(json.dumps(records,indent=2)+'\n')
if args.expect_red or args.expect_root_red:
 expected={'voted-alive','local-loss','collapsed','release-order','domain-failure','ahb-failure','child-release-failure','domain-release-failure'}
 if args.expect_root_red:expected={'root-active-0','root-active-1','root-active-2','root-active-3','roots-active','vsync-cmd-zero'}
 assert all({case for s,m,case in failures if s==soc and m==mode}==expected for soc in ('8250','8350') for mode in ('efi',)),failures
 print('PASS: authenticated expected OLD RED failures; unaffected strict controls already reject')
else: assert not failures,failures
