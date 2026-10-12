#!/usr/bin/env python3
"""Actual portable USB MSC core: BOT, SCSI, RAM FAT16, native/NT sanitizer builds."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p=Path(__file__).resolve().parents[1]; root=p.parents[1]
a=argparse.ArgumentParser(); a.add_argument('--output',type=Path,required=True)
o=a.parse_args().output.resolve(); o.mkdir(parents=True,exist_ok=True)
src=[p/'Tests/UsbMscTest.c',p/'Library/UsbMscCrLib/UsbMsc.c']
inputs=src+[Path(__file__),p/'Include/Library/UsbMscCrLib.h',p/'Include/oskal/cr_types.h',p/'Include/oskal/cr_status.h',p/'Include/oskal/cr_assert.h',p/'Tests/KernelMocks/ntddk.h',p/'Library/UsbMscCrLib/UsbMscCrLib.inf',p/'Library/UsbMscCrLib/UsbMscCrLib.props',p/'Library/UsbMscCrLib/README.md',p/'Library/UsbMscCrLib/LICENSE']
digest=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
frozen={str(f.relative_to(root)):digest(f) for f in inputs}; runs=[]
for kernel in (False,True):
    mode='nt' if kernel else 'native'; exe=o/mode
    cmd=['clang','-std=c17','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p/'Include')]
    cmd+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')] if kernel else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    cmd+=list(map(str,src))+['-o',str(exe)]
    c=subprocess.run(cmd,text=True,capture_output=True);(o/(mode+'-compile.log')).write_text(c.stdout+c.stderr)
    if c.returncode:print(c.stderr);c.check_returncode()
    run=[str(exe)]+([str(o/'ram-disk.img')] if not kernel else [])
    r=subprocess.run(run,text=True,capture_output=True,timeout=60);(o/(mode+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='');r.check_returncode();runs.append({'mode':mode,'command':cmd,'output':r.stdout,'binary_sha256':digest(exe)})
disk=o/'ram-disk.img';vol=o/'fat16.img';vol.write_bytes(disk.read_bytes()[2048*512:])
for label,cmd in [('fsck',['fsck.fat','-vn',str(vol)]),('mdir',['mdir','-i',str(disk)+'@@1048576','::']),('mtype',['mtype','-i',str(disk)+'@@1048576','::README.TXT'])]:
    r=subprocess.run(cmd,text=True,capture_output=True,timeout=30);(o/(label+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
    runs.append({'mode':label,'command':cmd,'output':r.stdout})
if frozen!={str(f.relative_to(root)):digest(f) for f in inputs}:raise RuntimeError('source drift')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'limitation':'Host core and external FAT utilities only; USB adapter, controller, cable and real host enumeration require device testing.','inputs':frozen,'runs':runs,'disk_sha256':digest(disk)},indent=2)+'\n')
