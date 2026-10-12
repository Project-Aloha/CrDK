#!/usr/bin/env python3
"""Actual portable DSI PHY shutdown, native and NT ASan/UBSan."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser(description=__doc__);a.add_argument('--output',type=Path,required=True);args=a.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
inputs=[p/'Tests/MdssPhyFenceTest.c',p/'Library/MdssPlatformLib/DsiPhy.c',p/'Library/MdssPlatformLib/MdssPlatformInternal.h',p/'Include/Library/MdssPlatformLib.h']
commands=[]
for nt in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
        '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p/'Include')]
 if nt:flags+=['-D_KERNEL_MODE','-I'+str(p/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 exe=out/('nt' if nt else 'native');cmd=flags+[str(x) for x in inputs[:2]]+['-o',str(exe)];commands.append(cmd)
 subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],check=True,text=True,capture_output=True,timeout=30)
 (out/(exe.name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout.strip())
(out/'artifacts.json').write_text(json.dumps({'scope':__doc__,'commands':commands,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
