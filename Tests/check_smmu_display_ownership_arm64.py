#!/usr/bin/env python3
"""Compile the actual reusable display ownership core and native DXE adapter."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path)
p.add_argument('--clang',default='/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang')
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
wdk=Path('/home/sunf/CrWDDK/Build/serial-kit/microsoft.windows.wdk.arm64/c/Include/10.0.26100.0')
sdk=Path('/home/sunf/CrWDDK/Build/serial-kit/microsoft.windows.sdk.cpp/c/Include/10.0.26100.0')
dirs=[wdk/'km',wdk/'km/crt',wdk/'shared',sdk/'shared'];overlay=out/'case-overlay.json'
overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[{'type':'directory','name':str(d),'contents':[{'type':'file','name':f.name,'external-contents':str(f)} for f in d.iterdir() if f.is_file()]} for d in dirs]}))
core=pkg/'Library/SmmuLib/SmmuDisplayOwnership.c';driver=pkg/'Driver/SmmuCrDxe/SmmuCrDxe.c'
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();deps={Path(__file__).resolve()};records=[]
for native,sources in [(True,[core,driver]),(False,[core])]:
 flags=['-ffreestanding','-std=c17','-Wall','-Wextra','-Werror','-I',str(pkg/'Include')]
 if native:flags+=['--target=aarch64-none-elf','-fshort-wchar','-I',str(root/'MU_BASECORE/MdePkg/Include'),'-I',str(root/'MU_BASECORE/MdePkg/Include/AArch64'),'-I',str(root/'MU_BASECORE/MdeModulePkg/Include'),'-D_PCD_VALUE_PcdUsbLegacyPhysicalDmaHandoff=FALSE','-D_PCD_VALUE_PcdUfsLegacyCoherentDmaHandoff=FALSE']
 else:
  flags+=['--target=aarch64-pc-windows-msvc','-fms-extensions','-D_ARM64_','-D_KERNEL_MODE','-D_WIN32_WINNT=0x0A00','-DNTDDI_VERSION=0x0A00000A','-Wno-microsoft-anon-tag','-ivfsoverlay',str(overlay),'-I','/home/sunf/CrWDDK/PepCr']
  for d in dirs:flags+=['-isystem',str(d)]
 for s in sources:
  raw=subprocess.check_output([a.clang,*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
  deps.update(Path(f).resolve() for f in shlex.split(raw.split(':',1)[1]) if Path(f).is_file())
  obj=out/(('efi-' if native else 'nt-')+s.stem+('.o' if native else '.obj'))
  before={str(d):sha(d) for d in deps};cmd=[a.clang,*flags,'-c',str(s),'-o',str(obj)]
  r=subprocess.run(cmd,text=True,capture_output=True);(out/(obj.name+'.log')).write_text(r.stdout+r.stderr)
  assert r.returncode==0,r.stderr;assert all(sha(Path(n))==h for n,h in before.items()),'source drift'
  records.append({'source':str(s.relative_to(root)),'object':str(obj),'command':cmd,'sha256':sha(obj)})
inputs={str(d.relative_to(root)) if d.is_relative_to(root) else str(d):sha(d) for d in sorted(deps)}
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'real EFI ARM64 DEBUG core+DXE and WDK ARM64 core objects; no link/package/hardware claim','objects':records,'inputs':inputs,'compiler':subprocess.check_output([a.clang,'--version'],text=True)},indent=2)+'\n')
print('PASS: actual EFI ARM64 DEBUG core + full DXE, actual WDK ARM64 reusable core')
