#!/usr/bin/env python3
"""Actual USBH/MDSH plus combo-PHY policy joint qualifier, EFI/NT sanitizer."""
from pathlib import Path
import argparse,hashlib,json,subprocess
cr=Path(__file__).resolve().parents[1];root=cr.parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[cr/'Tests/UsbDpPhyJointHandoffTest.c',cr/'Library/UsbDpPhyHandoffCrLib/UsbDpPhyHandoff.c',cr/'Library/UsbHandoffCrLib/UsbHandoff.c',cr/'Library/MdssHandoffCrLib/Handoff.c',cr/'Library/UsbDpPhyCrLib/UsbDpPhy.c']
inputs=sources+[Path(__file__),cr/'Tests/MdssHandoffTest.c',cr/'Tests/UsbDpPhyPortableTest.c',cr/'Tests/KernelMocks/ntddk.h',cr/'Library/UsbDpPhyCrLib/PhyTables.h']
inputs += [cr/'Include/Library'/n for n in ('UsbDpPhyHandoffCrLib.h','UsbDpPhyCrLib.h','UsbHandoffCrLib.h','MdssHandoffCrLib.h','smmu.h')]
inputs += [cr/'Include/Interface'/n for n in ('CrUsbHandoff.h','CrMdssHandoff.h')]
inputs += list((cr/'Library/UsbDpPhyHandoffCrLib').glob('*.inf'))+list((cr/'Library/UsbDpPhyHandoffCrLib').glob('*.props'))
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();frozen={str(f):sha(f) for f in inputs};runs=[]
for nt in (False,True):
 mode='nt' if nt else 'efi';exe=out/mode
 flags=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(cr/'Include')]
 if nt:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(cr/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 cmd=flags+list(map(str,sources))+['-o',str(exe)];r=subprocess.run(cmd,capture_output=True,text=True);(out/(mode+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 records=out/(mode+'-records');records.mkdir(exist_ok=True)
 r=subprocess.run([str(exe),str(records)],capture_output=True,text=True,timeout=60);(out/(mode+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();runs.append({'mode':mode,'command':cmd,'binary':sha(exe),'output':r.stdout,'records':{str(f):sha(f) for f in records.glob('*.bin')}})
assert frozen=={str(f):sha(f) for f in inputs},'source drift'
(out/'artifacts.json').write_text(json.dumps({'passed':True,'scope':__doc__,'limitations':'Pure native evidence qualification only. External NT/EFI primitives modeled; actual current-boot locator/RAM source identity, Windows assignment/D0/SPB/reset ownership and permanent baseline votes remain required. No hardware takeover grant from this test.','inputs':frozen,'runs':runs},indent=2)+'\n')
