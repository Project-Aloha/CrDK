#!/usr/bin/env python3
"""Actual charging UI and complete UEFI application under ASan/UBSan."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
inc=[c/'Include',r/'Platforms/MUCommonCrPkg/Include',r/'MU_BASECORE/MdePkg/Include',r/'MU_BASECORE/MdePkg/Include/X64']
records=[]
for fixture in ('ChargerUiTest.c','ChargerAppTest.c'):
 exe=out/Path(fixture).stem;cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMDEPKG_NDEBUG','-fsanitize=address,undefined','-fno-sanitize-recover=all',*[f'-I{x}' for x in inc],str(c/'Tests'/fixture),str(c/'Application/ChargerCr/ChargerUi.c'),'-o',str(exe)]
 run=subprocess.run(cmd,text=True,capture_output=True);(out/(exe.name+'.compile.log')).write_text(run.stdout+run.stderr)
 if run.returncode:print(run.stderr);raise SystemExit(run.returncode)
 run=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(out/(exe.name+'.log')).write_text(run.stdout+run.stderr)
 if run.returncode:print(run.stdout+run.stderr);raise SystemExit(run.returncode)
 print(run.stdout,end='');records.append({'fixture':fixture,'command':cmd,'result':run.stdout})
inputs=[c/'Application/ChargerCr'/x for x in ('ChargerCr.c','ChargerUi.c','ChargerUi.h','ChargerCr.inf')]+[c/'Tests'/x for x in ('ChargerUiTest.c','ChargerAppTest.c')]+[Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'actual charging app/UI/raster/timer/physical-output protocol call boundaries; hardware output and GLINK measured SOC not inferred','results':records,'inputs':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
