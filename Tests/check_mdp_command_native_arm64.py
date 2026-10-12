#!/usr/bin/env python3
"""Compile shared command native/event/power adapters for real EFI ARM64."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path)
p.add_argument('--clang',default='/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang')
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[pkg/'Driver/MDSSCrDxe'/s for s in ('MdpCommandNative.c','MdpNativeEvents.c','MdpPanelPower.c')];records=[];deps={Path(__file__).resolve()}
flags=['--target=aarch64-none-elf','-ffreestanding','-std=c17','-fshort-wchar','-DMDEPKG_NDEBUG','-Wall','-Wextra','-Werror','-fstack-usage','-I',str(pkg/'Include'),'-I',str(root/'MU_BASECORE/MdePkg/Include'),'-I',str(root/'MU_BASECORE/MdePkg/Include/AArch64'),'-I',str(root/'MU_BASECORE/EmbeddedPkg/Include'),'-I',str(root/'MU_BASECORE/MdeModulePkg/Include')]
for s in sources:
 obj=out/(s.stem+'.o');dep=obj.with_suffix('.d');cmd=[a.clang,*flags,'-MMD','-MF',str(dep),'-c',str(s),'-o',str(obj)]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(obj.name+'.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stderr);r.check_returncode()
 deps.update(Path(x).resolve() for x in shlex.split(dep.read_text().split(':',1)[1].replace('\\\n',' ')))
 records.append({'source':str(s.relative_to(root)),'object':str(obj),'command':cmd,'sha256':hashlib.sha256(obj.read_bytes()).hexdigest()})
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__+'; compile only, no linked frontend or hardware proof','objects':records,'inputs':{str(s.relative_to(root) if s.is_relative_to(root) else s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sorted(deps)},'compiler':subprocess.check_output([a.clang,'--version'],text=True)},indent=2)+'\n')
print('PASS: 3 actual shared command native EFI ARM64 objects')
