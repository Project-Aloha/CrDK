#!/usr/bin/env python3
"""Actual selected SM8250/SM8350 native target display graph controls."""
from pathlib import Path
import argparse,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);a.add_argument('--before',type=Path);args=a.parse_args()
o=args.output.resolve();o.mkdir(parents=True,exist_ok=True)
for soc,target in [('8250','CrClockTarget.c'),('8350','CrStorageClockTarget.c')]:
    flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
           '-fsanitize=address,undefined','-fno-sanitize-recover=all',
           '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
           '-DTEST_SOC=0x'+soc,'-I'+str(p/'Include')]
    flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    if args.before:flags+=['-DCR_DISPLAY_TARGET_PATH="'+str((args.before/'Platforms/CranePkg/Target'/('Sm'+soc)/target).resolve())+'"']
    exe=o/('display-'+soc+'-native')
    sources=[p/'Tests/ClockTargetDisplayTest.c',p/'Library/ClockLib/fixed_pll.c',
      p/'Library/ClockExternalLib/ClockExternal.c',p/'Library/ClockExternalLib/ClockExternalPlan.c',
      p/'Library/MdssHandoffCrLib/Handoff.c']
    cmd=flags+[str(s) for s in sources]+['-o',str(exe)]
    r=subprocess.run(cmd,capture_output=True,text=True);(o/('compile-'+soc+'-native'+'.log')).write_text(r.stdout+r.stderr)
    print(r.stderr,end='');r.check_returncode()
    r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);(o/('run-'+soc+'-native'+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='');r.check_returncode()
