#!/usr/bin/env python3
"""Staged USBH native observations/Windows ownership boundaries, actual core."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True)
o=a.parse_args().output.resolve();o.mkdir(parents=True,exist_ok=True)
src=[p/'Tests/UsbHandoffTest.c',p/'Library/UsbHandoffCrLib/UsbHandoff.c']
inputs=src+[Path(__file__),p/'Include/Interface/CrUsbHandoff.h',p/'Include/Library/UsbHandoffCrLib.h',p/'Tests/UsbHandoffMocks/ntddk.h',p/'Tests/KernelMocks/ntddk.h',p/'Include/Library/smmu.h',p/'Library/UsbHandoffCrLib/UsbHandoffCrLib.inf',p/'Library/UsbHandoffCrLib/UsbHandoffCrLib.props']
digest=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
frozen={str(f.relative_to(root)):digest(f) for f in inputs};runs=[]
for kernel in (False,True):
    mode='nt' if kernel else 'native';exe=o/mode
    cmd=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
    if kernel:cmd+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/UsbHandoffMocks'),'-I'+str(p/'Tests/KernelMocks')]
    else:cmd+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    cmd+=list(map(str,src))+['-o',str(exe)]
    c=subprocess.run(cmd,text=True,capture_output=True);(o/(mode+'-compile.log')).write_text(c.stdout+c.stderr)
    if c.returncode:print(c.stderr);c.check_returncode()
    r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(o/(mode+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='');r.check_returncode();runs.append({'mode':mode,'command':cmd,'output':r.stdout,'binary_sha256':digest(exe)})
if frozen!={str(f.relative_to(root)):digest(f) for f in inputs}:raise RuntimeError('source drift')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'limitation':'Unreferenced core only: no firmware final record, Windows URS/filter, assignment/power/PHY/DMA qualification, inbox package or hardware readiness supplied by these tests.','inputs':frozen,'runs':runs},indent=2)+'\n')
