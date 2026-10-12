#!/usr/bin/env python3
"""Compile the actual complete isolated command library, no hardware stubs."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
inputs=[pkg/'Tests/MdssCmdLifecycleTest.c']+sorted((pkg/'Library/MdssDsiCmdCrLib').glob('*.c'))+[pkg/'Library/MdssDscCrLib/MdssDsc.c']
for nt in (False,True):
    name='nt' if nt else 'native';exe=out/name
    flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections','-I'+str(pkg/'Include')]
    flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    r=subprocess.run(flags+list(map(str,inputs))+['-o',str(exe)],text=True,capture_output=True);(out/(name+'-compile.log')).write_text(r.stdout+r.stderr)
    if r.returncode:print(r.stderr);r.check_returncode()
    r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);(out/(name+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='');r.check_returncode()
inputs+=[Path(__file__),pkg/'Library/MdssDsiCmdCrLib/Internal.h',pkg/'Include/Library/MdssDsiCmdCrLib.h']
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','variants':['native','nt'],'inputs':{str(i.relative_to(root)):hashlib.sha256(i.read_bytes()).hexdigest() for i in inputs}},indent=2)+'\n')
