#!/usr/bin/env python3
"""Compile the battery core and both native transport/observer adapters for real ARM64 EFI/WDK."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path)
p.add_argument('--clang',default='/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang')
p.add_argument('--wdk',type=Path,default=Path('/home/sunf/CrWDDK/Build/serial-kit/microsoft.windows.wdk.arm64'))
p.add_argument('--sdk',type=Path,default=Path('/home/sunf/CrWDDK/Build/serial-kit/microsoft.windows.sdk.cpp'))
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
kit='10.0.26100.0';dirs=[a.wdk/'c/Include'/kit/'km',a.wdk/'c/Include'/kit/'km/crt',
 a.wdk/'c/Include'/kit/'shared',a.sdk/'c/Include'/kit/'shared']
overlay=out/'case-overlay.json';overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[
 {'type':'directory','name':str(d.resolve()),'contents':[
  {'type':'file','name':f.name,'external-contents':str(f.resolve())} for f in d.iterdir() if f.is_file()]}
 for d in dirs]}))
sources=[pkg/'Library/BatteryChargerCrLib/BatteryCharger.c',pkg/'Driver/UCSICrDxe/UCSICrDxe.c',pkg/'Driver/BatteryChargerCrDxe/BatteryChargerCrDxe.c']
records=[]
for native in (True,False):
 flags=['-ffreestanding','-std=c17','-Wall','-Wextra','-Werror','-I',str(pkg/'Include')]
 if native:flags+=['--target=aarch64-none-elf','-fshort-wchar','-DMDEPKG_NDEBUG',
  '-I',str(root/'MU_BASECORE/MdePkg/Include'),'-I',str(root/'MU_BASECORE/MdePkg/Include/AArch64'),'-I',str(root/'MU_BASECORE/EmbeddedPkg/Include')]
 else:
  flags+=['--target=aarch64-pc-windows-msvc','-fms-extensions','-D_ARM64_','-D_KERNEL_MODE',
   '-D_WIN32_WINNT=0x0A00','-DNTDDI_VERSION=0x0A00000A','-Wno-microsoft-anon-tag',
   '-ivfsoverlay',str(overlay),'-I','/home/sunf/CrWDDK/PepCr']
  for d in dirs:flags+=['-isystem',str(d)]
 for s in (sources if native else sources[:1]):
  obj=out/(('efi-' if native else 'nt-')+s.stem+('.o' if native else '.obj'))
  cmd=[a.clang,*flags,'-c',str(s),'-o',str(obj)];r=subprocess.run(cmd,text=True,capture_output=True)
  (out/(obj.name+'.log')).write_text(r.stdout+r.stderr)
  if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
  records.append({'source':str(s.relative_to(root)),'object':str(obj),'command':cmd,
   'sha256':hashlib.sha256(obj.read_bytes()).hexdigest()})
inputs=sources+[pkg/'Include/Library/BatteryChargerCrLib.h',pkg/'Include/Protocol/EFIBatteryChargerCrProtocol.h',pkg/'Include/Protocol/EFIPmicGlinkCrProtocol.h',Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'3 real EFI ARM64 adapter/core objects +1 actual WDK core object; no firmware/link/activation claim',
 'objects':records,'inputs':{str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in inputs},
 'compiler':subprocess.check_output([a.clang,'--version'],text=True)},indent=2)+'\n')
print('PASS: three actual EFI ARM64 + one actual WDK ARM64 object')
