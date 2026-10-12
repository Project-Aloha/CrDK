#!/usr/bin/env python3
"""Actual native USBH publisher and portable parser with modeled firmware services."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True)
out=a.parse_args().output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[p/'Tests/Dwc3HandoffTest.c',p/'Library/UsbHandoffCrLib/UsbHandoff.c']
inputs=sources+[Path(__file__),p/'Driver/Dwc3CrDxe/Dwc3Handoff.c',p/'Driver/Dwc3CrDxe/Dwc3Handoff.h',p/'Include/Interface/CrUsbHandoff.h',p/'Include/Protocol/EFISmmuCrProtocol.h',p/'Include/Library/UsbHandoffCrLib.h']
frozen={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all']
cmd+=['-I'+str(f) for f in [p/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']]
cmd+=list(map(str,sources))+['-o',str(out/'test')]
r=subprocess.run(cmd,capture_output=True,text=True);(out/'compile.log').write_text(r.stdout+r.stderr)
if r.returncode:print(r.stderr);r.check_returncode()
r=subprocess.run([str(out/'test'),str(out/'record.bin'),str(out/'locator.bin')],capture_output=True,text=True);(out/'run.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
assert frozen=={str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
(out/'artifacts.json').write_text(json.dumps({'passed':True,'scope':__doc__,'command':cmd,'inputs':frozen,'published_binary_pair':{n:hashlib.sha256((out/n).read_bytes()).hexdigest() for n in ('record.bin','locator.bin')}},indent=2)+'\n')
