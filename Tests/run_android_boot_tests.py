#!/usr/bin/env python3
"""Actual no-allocation Android boot parser + repository libfdt adversarial tests."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];edk=c.parents[1]/'MU_BASECORE'
base=edk/'MdePkg/Library/BaseFdtLib';fdt=base/'libfdt/libfdt'
sources=[c/'Tests/AndroidBootCrTest.c',c/'Library/AndroidBootCrLib/AndroidBoot.c',
         base/'FdtLib.c',*sorted(fdt.glob('*.c'))]
cmd=['clang','-std=c17','-D_GNU_SOURCE','-include','Uefi.h','-g','-O1','-fshort-wchar','-Wall','-Wextra',
 '-Werror','-Wno-unused-parameter','-Wno-sign-compare','-Wno-incompatible-pointer-types',
 '-fsanitize=address,undefined',
 '-fno-sanitize-recover=all','-I'+str(c/'Include'),'-I'+str(edk/'MdePkg/Include'),
 '-I'+str(edk/'MdePkg/Include/X64'),'-idirafter',str(base),'-I'+str(fdt),
 *map(str,sources),'-o',str(a.output/'android-boot')]
subprocess.run(cmd,check=True)
r=subprocess.run([str(a.output/'android-boot')],check=True,capture_output=True,text=True)
print(r.stdout,end='')
native=['clang','-target','aarch64-none-elf','-std=c17','-ffreestanding','-fshort-wchar',
 '-Wall','-Wextra','-Werror','-I'+str(edk/'MdePkg/Include'),
 '-I'+str(edk/'MdePkg/Include/AArch64'),'-I'+str(c/'Include'),'-c',
 str(c/'Library/AndroidBootCrLib/AndroidBoot.c'),'-o',str(a.output/'AndroidBoot.obj')]
subprocess.run(native,check=True)
inputs=[*sources,c/'Include/Library/AndroidBootCrLib.h',c/'Library/AndroidBootCrLib/AndroidBootCrLib.inf',Path(__file__)]
(a.output/'artifacts.json').write_text(json.dumps({'command':cmd,'output':r.stdout,
 'aarch64_command':native,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},
 'binary_sha256':hashlib.sha256((a.output/'android-boot').read_bytes()).hexdigest()},indent=2)+'\n')
