#!/usr/bin/env python3
"""Production ClockCrDxe + SM8250 target composition under real shared core."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--pll-source',type=Path);p.add_argument('--expect-main-rejection',action='store_true');a=p.parse_args()
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg';o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
s=[c/'Tests/ClockInheritedMainNativeTest.c',a.pll_source.resolve() if a.pll_source else c/'Library/ClockLib/fixed_pll.c',c/'Driver/ClockCrDxe/GpuBootAccess.c',*[c/'Library/ClockExternalLib'/n for n in ('ClockExternal.c','ClockExternalPlan.c','ClockExternalLease.c')]]
flags=['clang','-std=c17','-fshort-wchar','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-I'+str(c/'Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
deps={Path(__file__).resolve()}
for x in s:
 d=subprocess.check_output(flags+['-MM','-MT','test',str(x)],text=True);deps.update(Path(y).resolve() for y in shlex.split(d.split(':',1)[1].replace('\\\n',' ')))
exe=o/'native';cmd=flags+list(map(str,s))+['-Wl,--gc-sections','-o',str(exe)]
x=subprocess.run(cmd,capture_output=True,text=True);(o/'compile.log').write_text(x.stdout+x.stderr)
if x.returncode:print(x.stderr);x.check_returncode()
x=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);(o/'run.log').write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='')
if a.expect_main_rejection:
 assert x.returncode!=0 and 'AcquirePair' in x.stderr and 'ProtocolAcquireClock' in x.stderr,'not the expected actual MAIN-only acquisition rejection'
else:x.check_returncode()
(o/'artifacts.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_main_rejection else 'PASS','scope':__doc__+'; boundary MMIO, no hardware claim','command':cmd,'run_returncode':x.returncode,'inputs':{str(y.relative_to(r) if y.is_relative_to(r) else y):hashlib.sha256(y.read_bytes()).hexdigest() for y in sorted(deps)},'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'log_sha256':hashlib.sha256((o/'run.log').read_bytes()).hexdigest()},indent=2)+'\n')
