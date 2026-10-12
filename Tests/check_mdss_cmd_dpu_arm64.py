#!/usr/bin/env python3
"""Compile actual command DPU/DSC units and required cores with EFI/WDK ARM64 headers."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--wdk-workspace',type=Path,default=Path('/home/sunf/CrWDDK'))
a=p.parse_args()
r=Path(__file__).resolve().parents[3]; c=r/'Platforms/CranePkg'; o=a.output.resolve()
o.mkdir(parents=True,exist_ok=True); (o/'artifacts.json').unlink(missing_ok=True)
compiler=Path('/home/sunf/droidvm-repos/out/artifacts/dxvk/llvm-mingw-20260908-ucrt-ubuntu-22.04-x86_64/bin/clang-23')
kit=a.wdk_workspace.resolve()/'Build/serial-kit'
wdk=kit/'microsoft.windows.wdk.arm64/c/Include/10.0.26100.0'
sdk=kit/'microsoft.windows.sdk.cpp/c/Include/10.0.26100.0'
include=[wdk/'km',wdk/'km/crt',wdk/'shared',sdk/'shared',sdk/'um']
overlay=o/'case-overlay.json'
overlay.write_text(json.dumps({'version':0,'case-sensitive':False,'roots':[{'type':'directory','name':str(d),'contents':[{'type':'file','name':str(f.relative_to(d)),'external-contents':str(f)} for f in d.rglob('*') if f.is_file()]} for d in include]}))
sources=[c/'Library/MdssDsiCmdCrLib'/n for n in ('DscLegacy.c','DpuCommand.c','Io.c','Plan.c','PanelR66451.c')]
(o/'trace.h').write_text('#pragma once\n/* Compile-only WPP substitution; OS/IO APIs unchanged. */\n#define log_raw(...) ((void)0)\n#define log_info(...) ((void)0)\n#define log_warn(...) ((void)0)\n#define log_err(...) ((void)0)\n')
sources += [c/'Library/MdssDscCrLib/MdssDsc.c']
inputs=sources+[c/'Include/Library/MdssDsiCmdCrLib.h',c/'Include/Library/MdssDscCrLib.h',c/'Library/MdssDsiCmdCrLib/Internal.h',Path(__file__)]
before={str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
commands=[]; outputs=[]; dependencies=set()
for nt in (False,True):
    name='wdk' if nt else 'efi'; flags=[str(compiler),'-ffreestanding','-fshort-wchar','-std=c17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-I'+str(c/'Include')]
    if nt:
        flags+=['--target=aarch64-pc-windows-msvc','-fms-extensions','-Wno-microsoft-anon-tag','-D_ARM64_','-D_KERNEL_MODE','-D_WIN32_WINNT=0x0A00','-DNTDDI_VERSION=0x0A00000A','-ivfsoverlay',str(overlay),'-I'+str(o)]
        for d in include: flags+=['-isystem',str(d)]
    else:
        flags+=['--target=aarch64-none-elf','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/AArch64')]
    for f in sources:
        obj=o/(name+'-'+f.stem+('.obj' if nt else '.o')); dep=obj.with_suffix('.d')
        cmd=flags+['-MMD','-MF',str(dep),'-c',str(f),'-o',str(obj)]; commands.append(cmd)
        x=subprocess.run(cmd,text=True,capture_output=True)
        obj.with_suffix('.log').write_text(x.stdout+x.stderr)
        if x.returncode: print(x.stderr); x.check_returncode()
        outputs.append(obj)
        raw=dep.read_text().replace('\\\n',' ')
        dependencies.update(Path(t) for t in raw.split(':',1)[1].split())
after={str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
if before!=after: raise RuntimeError('source changed during compile')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'5 actual sources x ARM64 EFI and WDK objects; compile-only, no active registration/full platform link/hardware claim','inputs':after,'compiler':{'path':str(compiler),'sha256':hashlib.sha256(compiler.read_bytes()).hexdigest(),'version':subprocess.check_output([str(compiler),'--version'],text=True)},'dependencies':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(dependencies) if f.is_file()},'objects':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in outputs},'commands':commands},indent=2)+'\n')
print('actual EFI+WDK ARM64 10 objects PASS')
