#!/usr/bin/env python3
"""Actual guarded DPU reflection source, native/NT sanitizer hostile IO."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3];c=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[c/'Tests/MdssDpuFetchTest.c',c/'Library/MdssDpu2dCrLib/MdssDpuFetch.c',c/'Library/MdssCrLib/Mdss.c']
results=[]
for nt in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 exe=out/('nt' if nt else 'native');cmd=flags+list(map(str,sources))+['-Wl,--gc-sections','-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(exe.name+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(out/(exe.name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();results.append(exe.name)
files=sources+[c/'Include/Library/MdssDpu2dCrLib.h',Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'actual native/NT DPU fetch control with independently modeled MMIO and faults; no hardware runtime claim','results':results,'inputs':{str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files}},indent=2)+'\n')
