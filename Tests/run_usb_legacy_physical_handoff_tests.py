#!/usr/bin/env python3
"""Compose actual USBH + native SMMU + portable SMMU; model only services/MMIO."""
from pathlib import Path
import argparse
import hashlib
import json
import shlex
import shutil
import subprocess

p=Path(__file__).resolve().parents[1]
root=p.parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output',type=Path,required=True)
parser.add_argument('--staged-root',type=Path)
args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
core=(args.staged_root.resolve()/'Platforms/CranePkg') if args.staged_root else p
sources=[p/'Tests/UsbLegacyPhysicalHandoffTest.c',p/'Library/UsbHandoffCrLib/UsbHandoff.c',core/'Library/SmmuLib/smmu.c']
inputs=sources+[Path(__file__),p/'Tests/Dwc3HandoffTest.c',p/'Driver/Dwc3CrDxe/Dwc3Handoff.c',p/'Driver/Dwc3CrDxe/Dwc3Handoff.h',p/'Driver/SmmuCrDxe/SmmuCrDxe.c',p/'Include/Interface/CrUsbHandoff.h',p/'Include/Protocol/EFISmmuCrProtocol.h',p/'Include/Library/UsbHandoffCrLib.h',core/'Include/Library/smmu.h']
include_flags=['-I'+str(f) for f in [core/'Include',p/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']]
# Freeze actual selected repository headers before either compile, including
# transitive EDK2 definitions; system C headers stay toolchain-owned.
selected=set(inputs)
for release in (False,True):
    for source in sources:
        dep=['clang','-std=c11','-fshort-wchar','-MM']+include_flags
        if release:dep+=['-DMDEPKG_NDEBUG']
        r=subprocess.run(dep+[str(source)],capture_output=True,text=True,check=True)
        selected.update(Path(s).resolve() for s in shlex.split(r.stdout.replace('\\\n',' ').partition(':')[2]))
inputs=sorted(selected)
def hashes():
    return {str(f.relative_to(root)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
frozen=hashes()
for f in inputs:
    archived=out/'inputs'/f.relative_to(root);archived.parent.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(f,archived)
commands=[]
for variant in ('DEBUG','RELEASE'):
    dest=out/variant;dest.mkdir(exist_ok=True)
    cmd=['clang','-std=c11','-fshort-wchar','-g','-O1' if variant=='DEBUG' else '-O2','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections']
    if variant=='RELEASE':cmd+=['-DMDEPKG_NDEBUG']
    cmd+=include_flags
    cmd+=list(map(str,sources))+['-o',str(dest/'test')];commands.append(cmd)
    r=subprocess.run(cmd,capture_output=True,text=True);(dest/'compile.log').write_text(r.stdout+r.stderr)
    if r.returncode:print(r.stdout+r.stderr,end='');r.check_returncode()
    r=subprocess.run([str(dest/'test')],capture_output=True,text=True);(dest/'run.log').write_text(r.stdout+r.stderr)
    print(variant+': '+r.stdout+r.stderr,end='');r.check_returncode()
assert hashes()==frozen,'Selected source changed during tests'
(out/'receipt.json').write_text(json.dumps({'passed':True,'scope':__doc__,'variants':['DEBUG','RELEASE'],'inputs':frozen,'commands':commands},indent=2)+'\n')
