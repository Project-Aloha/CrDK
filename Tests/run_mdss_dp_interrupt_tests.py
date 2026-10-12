#!/usr/bin/env python3
"""Actual combined MDSS DPU/DP ledger in native/NT sanitizers; controlled MMIO."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg';o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
sources=[c/'Tests/MdssDpInterruptTest.c',*[c/'Library/MdssDpuInterruptCrLib'/n for n in ('MdssDpuInterrupt.c','MdssDpInterrupt.c','MdssIrqSources.c')]]
inputs=sources+[Path(__file__),*[c/'Include/Library'/n for n in ('MdssDpuInterruptCrLib.h','MdssDpInterruptCrLib.h','MdssIrqSourcesCrLib.h')]]
digest=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();before={str(f):digest(f) for f in inputs};runs=[]
for nt in (False,True):
 name='nt' if nt else 'native';flags=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
 cmd=flags+list(map(str,sources))+['-o',str(o/name)];x=subprocess.run(cmd,text=True,capture_output=True);(o/(name+'-compile.log')).write_text(x.stdout+x.stderr);x.check_returncode()
 x=subprocess.run([str(o/name)],text=True,capture_output=True,timeout=30);(o/(name+'.log')).write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode();runs.append({'command':cmd,'status':x.returncode})
if before!={str(f):digest(f) for f in inputs}:raise RuntimeError('source drift')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':before,'runs':runs},indent=2)+'\n')
