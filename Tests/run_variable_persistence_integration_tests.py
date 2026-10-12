#!/usr/bin/env python3
"""Run actual emulated-store bootstrap/parser and DXE persistence adapter."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import shlex
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
cr=Path(__file__).resolve().parents[1];root=cr.parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
includes=[cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']
source=cr/'Tests/VariablePersistenceIntegrationTest.c'
runs=[]
deps=set();outputs=[]
for debug in (True,False):
 exe=out/('debug' if debug else 'release')
 dep=exe.with_suffix('.d')
 cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all']
 if not debug:cmd+=['-DMDEPKG_NDEBUG']
 cmd+=['-MD','-MF',str(dep)]+['-I'+str(i) for i in includes]+[str(source),'-o',str(exe)]
 build=subprocess.run(cmd,text=True,capture_output=True)
 buildlog=exe.with_name(exe.name+'-compile.log');buildlog.write_text(build.stdout+build.stderr);build.check_returncode()
 compiled={Path(f).resolve() for f in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])}
 assert cr/'Include/Library/VariablePersistenceLib.h' in compiled
 assert not any('/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe/' in str(f) and '/Override/' not in str(f) for f in compiled)
 deps.update(compiled)
 r=subprocess.run([str(exe)],text=True,capture_output=True);print(r.stdout+r.stderr,end='');r.check_returncode()
 runlog=exe.with_name(exe.name+'-run.log');runlog.write_text(r.stdout+r.stderr)
 outputs.extend([exe,dep,buildlog,runlog])
 runs.append({'command':cmd,'output':r.stdout})
v=cr/'Override/MU_BASECORE/MdeModulePkg/Universal/Variable/RuntimeDxe'
inputs={source,Path(__file__).resolve(),v/'VariableRuntimeDxe.inf',v/'SOURCE_INVENTORY.json',*deps}
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
(out/'artifacts.json').write_text(json.dumps({'scope':'Actual staged bootstrap/parser and DXE adapter; modeled variable mutation and persistence IO, no physical reboot test','sources':{str(f):sha(f) for f in sorted(inputs)},'outputs':{str(f):sha(f) for f in outputs},'runs':runs},indent=2)+'\n')
