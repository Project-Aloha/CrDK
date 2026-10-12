#!/usr/bin/env python3
"""Whole actual SM8250 boot owner against source-qualified route/IRQ seed."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--oracle',type=Path,required=True)
p.add_argument('--oracle-receipt',type=Path,required=True)
p.add_argument('--clang',default='/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang')
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);commands=[];frozen={}
digest=lambda q:hashlib.sha256(q.read_bytes()).hexdigest()
def pin(q):
 q=q.resolve();d=digest(q)
 assert q not in frozen or frozen[q]==d,q
 frozen[q]=d
 return q
def run(cmd,log=None,**kw):
 commands.append(list(map(str,cmd)));r=subprocess.run(cmd,text=True,capture_output=True,**kw)
 if log:(out/log).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
 return r
def deps(dep):
 for q in dep.read_text().split(':',1)[1].replace('\\\n','').split():pin(Path(q))
receipt=json.loads(a.oracle_receipt.read_text());assert receipt['status']=='PASS'
for q,d in receipt['inputs'].items():
 q=Path(q);q=q if q.is_absolute() else root/q
 assert digest(q)==d, q
 pin(q)
pin(a.oracle_receipt);pin(a.oracle);pin(Path(__file__))
sources=[pkg/'Library/MdssBoot8250CrLib'/name for name in ('Io.c','Observe.c','Retire.c')]
inputs=[pkg/'Tests/MdssBoot8250Test.c',a.oracle.resolve(),*sources]
for q in inputs:pin(q)
for nt in (False,True):
 mode='nt' if nt else 'native'
 flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-misleading-indentation',
  '-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(pkg/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 objs=[]
 for i,q in enumerate(inputs):
  obj=out/f'{mode}-{i}.o';dep=out/f'{mode}-{i}.d'
  run([*flags,'-MM','-MF',dep,q]);deps(dep)
  # The unchanged Android decoder shifts a signed mask into bit31. Keep its
  # exact body; production and test units retain every sanitizer check.
  extra=['-fno-sanitize=shift-base'] if q==a.oracle.resolve() else []
  run([*flags,*extra,'-c',q,'-o',obj],f'{mode}-{i}-compile.log');objs.append(obj)
 exe=out/mode;run(['clang','-fsanitize=address,undefined',*objs,'-o',exe],f'{mode}-link.log')
 r=run([exe],f'{mode}.log',timeout=90);print(mode+': '+r.stdout.strip())
kit=Path('/home/sunf/CrWDDK/Build/serial-kit');version='10.0.26100.0'
wdk=kit/'microsoft.windows.wdk.arm64/c/Include'/version
sdk=kit/'microsoft.windows.sdk.cpp/c/Include'/version
includes=[wdk/'km',wdk/'km/crt',wdk/'shared',sdk/'shared']
overlay=out/'case-overlay.json';overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[
 {'type':'directory','name':str(d),'contents':[{'type':'file','name':f.name,'external-contents':str(f)} for f in d.iterdir() if f.is_file()]} for d in includes]}))
for nt in (False,True):
 mode='wdk-arm64' if nt else 'efi-arm64'
 flags=[a.clang,'-ffreestanding','-std=c17','-Wall','-Wextra','-Werror','-fstack-usage','-I',str(pkg/'Include')]
 if nt:
  flags+=['--target=aarch64-pc-windows-msvc','-fms-extensions','-D_ARM64_','-D_KERNEL_MODE','-D_WIN32_WINNT=0x0A00',
   '-DNTDDI_VERSION=0x0A00000A','-Wno-microsoft-anon-tag','-ivfsoverlay',str(overlay),'-I','/home/sunf/CrWDDK/PepCr']
  for d in includes:flags+=['-isystem',str(d)]
 else:flags+=['--target=aarch64-none-elf','-fshort-wchar','-DMDEPKG_NDEBUG','-I',str(root/'MU_BASECORE/MdePkg/Include'),'-I',str(root/'MU_BASECORE/MdePkg/Include/AArch64')]
 for q in sources:
  obj=out/f'{mode}-{q.stem}.o';dep=out/f'{mode}-{q.stem}.d'
  run([*flags,'-M','-MF',dep,q]);deps(dep)
  run([*flags,'-c',q,'-o',obj],f'{mode}-{q.stem}.log')
for q,d in frozen.items():assert digest(q)==d,q
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__+'; native/NT sanitizer and 3+3 real ARM64 objects; native binding and hardware remain separate',
 'commands':commands,'inputs':{str(q):d for q,d in sorted(frozen.items())},
 'artifacts':{q.name:digest(q) for q in out.iterdir() if q.is_file() and q.name!='artifacts.json'}},indent=2)+'\n')
print('PASS: complete actual boot owner and 6 real ARM64 production objects')
