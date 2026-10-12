#!/usr/bin/env python3
"""Actual owned TOP0 source in native/NT sanitizer builds; no physical IRQ claim."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3];c=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[c/'Tests/MdssDpuInterruptTest.c',c/'Library/MdssDpuInterruptCrLib/MdssDpuInterrupt.c',c/'Library/MdssDpuInterruptCrLib/MdssDpInterrupt.c',c/'Library/MdssDpuInterruptCrLib/MdssDpLinkInterrupt.c',c/'Library/MdssDpuInterruptCrLib/MdssIrqSources.c']
runs=[]
for nt in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 name='nt' if nt else 'native';exe=out/name;cmd=flags+list(map(str,sources))+['-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(name+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(out/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
 runs.append({'name':name,'command':cmd,'exit':r.returncode})
files=sources+[c/'Include/Library/MdssDpuInterruptCrLib.h',Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'actual TOP0 IRQ ledger; independent register/W1C model; no physical IRQ, parent grant, WB kick or DMA fence','runs':runs,'inputs':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}},indent=2)+'\n')
