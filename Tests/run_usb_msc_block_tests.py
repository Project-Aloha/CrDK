#!/usr/bin/env python3
"""Actual MSC block backend with sparse media, faults and native/NT sanitizers."""
import argparse, hashlib, json, shlex, subprocess
from pathlib import Path
crane=Path(__file__).resolve().parents[1];root=crane.parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);args=p.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[crane/'Tests/UsbMscBlockTest.c',crane/'Library/UsbMscCrLib/UsbMsc.c']
H=lambda p:hashlib.sha256(Path(p).read_bytes()).hexdigest()
inputs={str(Path(__file__).resolve()):H(__file__)};runs=[]
for mode,kernel in [('native',False),('nt',True)]:
 flags=['-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(crane/'Include')]
 flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')] if kernel else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 for s in sources:
  dep=subprocess.check_output(['clang',*flags,'-MM','-MT','test',str(s)],text=True)
  for name in shlex.split(dep.split(':',1)[1].replace('\\\n',' ')):
   f=str(Path(name).resolve());h=H(f);assert f not in inputs or inputs[f]==h,'source drift between modes';inputs[f]=h
 exe=out/mode;cmd=['clang',*flags,*map(str,sources),'-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(mode+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode: print(r.stdout+r.stderr,end='');r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);log=out/(mode+'.log');log.write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
 runs.append({'mode':mode,'command':cmd,'returncode':r.returncode,'executable_sha256':H(exe),'log_sha256':H(log)})
 assert all(H(f)==h for f,h in inputs.items()),'source drift during compile/run'
 (out/'artifacts.json').write_text(json.dumps({'scope':__doc__,'inputs':inputs,'runs':runs,'status':'PASS' if r.returncode==0 else 'FAIL'},indent=2)+'\n')
 r.check_returncode()
print('Receipt',out/'artifacts.json',H(out/'artifacts.json'))
