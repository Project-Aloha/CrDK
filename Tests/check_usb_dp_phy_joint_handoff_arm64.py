#!/usr/bin/env python3
"""Real EFI/WDK ARM64 object compile of the joint qualifier and validators."""
from pathlib import Path
import argparse,hashlib,json,subprocess
cr=Path(__file__).resolve().parents[1];root=cr.parents[1]
p=argparse.ArgumentParser();p.add_argument('--clang',type=Path,required=True);p.add_argument('--wdk-receipt',type=Path,required=True);p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[cr/'Library/UsbDpPhyHandoffCrLib/UsbDpPhyHandoff.c',cr/'Library/UsbHandoffCrLib/UsbHandoff.c',cr/'Library/MdssHandoffCrLib/Handoff.c']
inputs=sources+[Path(__file__),a.wdk_receipt]
inputs += [cr/'Include/Library'/n for n in ('UsbDpPhyHandoffCrLib.h','UsbDpPhyCrLib.h','UsbHandoffCrLib.h','MdssHandoffCrLib.h','smmu.h')]
inputs += [cr/'Include/Interface'/n for n in ('CrUsbHandoff.h','CrMdssHandoff.h')]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();frozen={str(f):sha(f) for f in inputs};objects=[]
wdkflags=json.loads(a.wdk_receipt.read_text())['flags']
for mode in ('efi','nt'):
 if mode=='efi':flags=['--target=aarch64-none-elf','-ffreestanding','-std=c17','-Wall','-Wextra','-Werror','-fshort-wchar','-DMDEPKG_NDEBUG','-I'+str(cr/'Include'),'--sysroot=/nonexistent','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/AArch64')]
 else:flags=['-I'+str(cr/'Include'),*wdkflags]
 for s in sources:
  obj=out/(mode+'-'+s.parent.name+'.obj');cmd=[str(a.clang),*flags,'-c',str(s),'-o',str(obj)]
  r=subprocess.run(cmd,text=True,capture_output=True);(out/(obj.name+'.log')).write_text(r.stdout+r.stderr)
  if r.returncode:print(r.stderr);r.check_returncode()
  objects.append({'mode':mode,'source':str(s),'command':cmd,'object':str(obj),'sha256':sha(obj)})
assert frozen=={str(f):sha(f) for f in inputs},'source drift'
(out/'artifacts.json').write_text(json.dumps({'passed':True,'scope':__doc__,'limitation':'Three real EFI objects plus three real WDK objects. No MSBuild substitution, whole driver link, firmware activation or hardware ownership claimed.','inputs':frozen,'objects':objects},indent=2)+'\n')
print('PASS real ARM64 EFI/WDK joint PHY qualifier + USBH/MDSH validators (6 objects)')
