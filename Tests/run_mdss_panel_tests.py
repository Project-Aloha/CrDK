#!/usr/bin/env python3
"""Actual immutable panel caller + dual DSI/CPHY/DSC compositor lifecycle."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);o=p.parse_args().output.resolve();o.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg'
s=[c/'Tests/MdssDsiPanelTest.c',*[c/f'Library/MdssDsiCrLib/{n}.c' for n in ('MdssDsi','MdssDsiCommands','MdssDsiClock','MdssDsiPanel','DsiPhyExtended')],c/'Library/MdssDualCrLib/MdssDual.c',c/'Library/MdssDscCrLib/MdssDsc.c',c/'Library/MdssCrLib/Mdss.c',c/'Library/MdssCrLib/MdssHardware.c',c/'Library/ClockExternalLib/ClockExternalPlan.c']
for k in (False,True):
 name='nt' if k else 'native';exe=o/name
 cmd=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections','-I'+str(c/'Include')]
 if k:cmd+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')]
 else:cmd+=['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
 cmd+=[str(x) for x in s]+['-o',str(exe)]
 x=subprocess.run(cmd,text=True,capture_output=True);(o/(name+'-compile.log')).write_text(x.stdout+x.stderr)
 if x.returncode:print(x.stderr);x.check_returncode()
 x=subprocess.run([str(exe)],text=True,capture_output=True,timeout=90);(o/(name+'.log')).write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
inputs=s+[Path(__file__),c/'Tests/MdssDsiExtendedTest.c',c/'Include/Library/MdssDsiPanelCrLib.h',c/'Include/Library/MdssDualCrLib.h',c/'Include/Library/MdssDsiCrLib.h']
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
