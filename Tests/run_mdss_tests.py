#!/usr/bin/env python3
"""Run actual portable MDSS sources with native and NT types under sanitizers."""
from pathlib import Path
import subprocess
import tempfile
import argparse,contextlib,hashlib,json
p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path);args=parser.parse_args()
if args.output:args.output.resolve().mkdir(parents=True,exist_ok=True)
context=contextlib.nullcontext(str(args.output.resolve())) if args.output else tempfile.TemporaryDirectory(prefix='crane-mdss-')
with context as d:
    records=[]
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p/'Include')]
        if kernel: flags+=['-D_KERNEL_MODE','-I'+str(p/'Tests/KernelMocks')]
        else: flags+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),
                      '-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        exe=Path(d)/('nt' if kernel else 'native')
        subprocess.run(flags+[str(p/'Tests/MdssCrTest.c'),str(p/'Library/MdssCrLib/Mdss.c'),
                              str(p/'Library/MdssCrLib/MdssPresent.c'),'-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],check=True,timeout=30,text=True,capture_output=True)
        print(r.stdout,end='');records.append({'abi':'nt' if kernel else 'native','output':r.stdout})
    if args.output:
        files=[p/'Tests/MdssCrTest.c',p/'Library/MdssCrLib/Mdss.c',p/'Library/MdssCrLib/MdssPresent.c',p/'Include/Library/MdssCrLib.h',Path(__file__)]
        (Path(d)/'artifacts.json').write_text(json.dumps({'scope':'actual native+NT core ASan/UBSan, exact synchronous ownership and checked IO failure retention','results':records,'inputs':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}},indent=2)+'\n')
