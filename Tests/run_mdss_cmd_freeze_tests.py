#!/usr/bin/env python3
"""Actual command EBS physical fence and retained failure journals."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);a=p.parse_args()
root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[pkg/'Tests/MdssCmdFreezeTest.c',*sorted((pkg/'Library/MdssDsiCmdCrLib').glob('*.c')),pkg/'Library/MdssDscCrLib/MdssDsc.c'];deps={Path(__file__).resolve()};records=[]
for nt in (False,True):
 name='nt' if nt else 'native';flags=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-I'+str(pkg/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 for src in sources:
  d=subprocess.check_output(flags+['-MM','-MT','test',str(src)],text=True);deps.update(Path(x).resolve() for x in shlex.split(d.split(':',1)[1].replace('\\\n',' ')))
 exe=out/name;cmd=flags+list(map(str,sources))+['-Wl,--gc-sections','-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(name+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(out/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
 records.append({'command':cmd,'exit':r.returncode,'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'log_sha256':hashlib.sha256((out/(name+'.log')).read_bytes()).hexdigest()})
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__+'; deterministic external IO, no native/EBS producer or hardware claim','cases':records,
 'inputs':{str(x.relative_to(root) if x.is_relative_to(root) else x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(deps)}},indent=2)+'\n')
