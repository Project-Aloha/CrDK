#!/usr/bin/env python3
"""Compile shared clock algorithms for real EFI/WDK ARM64, selected targets for EFI."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path)
p.add_argument('--clang',default='/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang')
p.add_argument('--wdk',type=Path,default=Path('/home/sunf/CrWDDK/Build/serial-kit/microsoft.windows.wdk.arm64'))
p.add_argument('--sdk',type=Path,default=Path('/home/sunf/CrWDDK/Build/serial-kit/microsoft.windows.sdk.cpp'))
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
kit='10.0.26100.0';dirs=[a.wdk/'c/Include'/kit/'km',a.wdk/'c/Include'/kit/'km/crt',a.wdk/'c/Include'/kit/'shared',a.sdk/'c/Include'/kit/'shared',a.wdk/'c/Include/wdf/kmdf/1.33']
overlay=out/'case-overlay.json';overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[
 {'type':'directory','name':str(d.resolve()),'contents':[{'type':'file','name':f.name,'external-contents':str(f.resolve())} for f in d.iterdir() if f.is_file()]} for d in dirs]}))
portable=[pkg/'Library/ClockExternalLib'/f for f in ('ClockExternal.c','ClockExternalPlan.c','ClockExternalLease.c')]+[
 pkg/'Library/ClockLib'/f for f in ('clock.c','clock_config.c','gdsc.c','clock_votes.c','fixed_pll.c')]
efi_only=[pkg/'Target/Sm8250/CrClockTarget.c',pkg/'Target/Sm8350/CrStorageClockTarget.c',pkg/'Driver/ClockCrDxe/ClockCrDxe.c',pkg/'Driver/ClockCrDxe/GpuBootAccess.c',pkg/'Driver/MDSSCrDxe/MdpNativeResources.c']
records=[];inputs=set(portable+efi_only+[Path(__file__)])
for native in (True,False):
 flags=['-ffreestanding','-std=c17','-Wall','-Wextra','-Werror','-I',str(pkg/'Include')]
 if native:flags+=['--target=aarch64-none-elf','-fshort-wchar','-I',str(root/'MU_BASECORE/MdePkg/Include'),'-I',str(root/'MU_BASECORE/MdePkg/Include/AArch64'),'-I',str(root/'MU_BASECORE/MdeModulePkg/Include')]
 else:
  flags+=['--target=aarch64-pc-windows-msvc','-fms-extensions','-D_ARM64_','-D_KERNEL_MODE','-D_WIN32_WINNT=0x0A00','-DNTDDI_VERSION=0x0A00000A','-Wno-microsoft-anon-tag','-ivfsoverlay',str(overlay),'-I','/home/sunf/CrWDDK/PepCr']
  for d in dirs:flags+=['-isystem',str(d)]
 for s in portable+(efi_only if native else []):
  obj=out/(('efi-' if native else 'nt-')+s.stem+('.o' if native else '.obj'));dep=out/(obj.name+'.d')
  cmd=[a.clang,*flags,'-MMD','-MF',str(dep),'-c',str(s),'-o',str(obj)];r=subprocess.run(cmd,text=True,capture_output=True)
  (out/(obj.name+'.log')).write_text(r.stdout+r.stderr)
  if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
  for item in dep.read_text().replace('\\\n',' ').split(':',1)[1].split():
   q=Path(item)
   if q.exists() and q.is_relative_to(root):inputs.add(q)
  records.append({'source':str(s.relative_to(root)),'object':str(obj),'command':cmd,'sha256':hashlib.sha256(obj.read_bytes()).hexdigest()})
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'21 DEBUG source objects, no firmware linking or runtime claim','objects':records,
 'inputs':{str(s.relative_to(root)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sorted(inputs)},'compiler':subprocess.check_output([a.clang,'--version'],text=True)},indent=2)+'\n')
print('PASS: 13 actual EFI ARM64 + 8 actual WDK ARM64 clock/resource objects')
