#!/usr/bin/env python3
"""Actual HDK platform source tests, native and NT types under ASAN/UBSAN."""
from pathlib import Path
import argparse,contextlib,hashlib,json,subprocess,tempfile
p=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',type=Path);a=parser.parse_args()
if a.output:a.output.resolve().mkdir(parents=True,exist_ok=True)
ctx=contextlib.nullcontext(str(a.output.resolve())) if a.output else tempfile.TemporaryDirectory(prefix='crane-mdss-platform-')
runs=[]
with ctx as d:
    for kernel in (False,True):
        flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror',
          '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p/'Include')]
        if kernel: flags+=['-D_KERNEL_MODE','-I'+str(p/'Tests/KernelMocks')]
        else: flags+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),'-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        sources=[p/'Tests/MdssPlatformTest.c',p/'Library/MdssCrLib/Mdss.c']
        sources += [p/'Library/MdssPlatformLib'/n for n in ('Platform.c','DsiPhy.c','Lt9611.c')]
        exe=Path(d)/('nt' if kernel else 'native')
        subprocess.run(flags+list(map(str,sources))+['-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],check=True,timeout=30,capture_output=True,text=True);print(r.stdout,end='')
        runs.append({'variant':'nt' if kernel else 'native','command':flags+list(map(str,sources))+['-o',str(exe)],'output':r.stdout})
    if a.output:
        files=sources+[p/'Include/Library/MdssPlatformLib.h',p/'Include/Library/MdssCrLib.h',Path(__file__)]
        (Path(d)/'artifacts.json').write_text(json.dumps({'scope':'actual platform/core native and NT ASan/UBSan; real partial ownership, resources-only admission, explicit POST reclaim; provider boundaries controlled, no runtime claim','runs':runs,'sources':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}},indent=2)+'\n')
