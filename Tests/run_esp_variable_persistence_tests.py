#!/usr/bin/env python3
"""Build the complete actual EFI ESP backend against real protocol headers."""
import argparse, hashlib, json, subprocess, shlex
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);a=p.parse_args()
a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
crane=Path(__file__).resolve().parents[1];root=crane.parents[1];edk=root/'MU_BASECORE'
source=crane/'Tests/EspVariablePersistenceTest.c';backend=crane/'Library/EspVariablePersistenceLib/EspVariablePersistence.c'
includes=[crane/'Include',edk/'MdePkg/Include',edk/'MdePkg/Include/X64',edk/'MdeModulePkg/Include',edk/'EmbeddedPkg/Include']
binary=a.output/'esp-variable-persistence'
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all']
hostdep=a.output/'host.d'
cmd+=['-MD','-MF',str(hostdep)]+['-I'+str(d) for d in includes]+[str(source),'-o',str(binary)]
build=subprocess.run(cmd,text=True,capture_output=True);hostlog=a.output/'host-compile.log';hostlog.write_text(build.stdout+build.stderr);build.check_returncode()
r=subprocess.run([str(binary)],capture_output=True,text=True);print(r.stdout,end='');print(r.stderr,end='');r.check_returncode()
runlog=a.output/'host-run.log';runlog.write_text(r.stdout+r.stderr)
arm=a.output/'esp-variable-persistence-aarch64.o'
armcmd=['clang','--target=aarch64-none-elf','-ffreestanding','-fshort-wchar','-fno-builtin','-std=c11','-DMDEPKG_NDEBUG','-Wall','-Wextra','-Werror','-Wno-unused-parameter']
armdep=a.output/'arm64.d'
armcmd+=['-MD','-MF',str(armdep)]+['-I'+str(d) for d in [crane/'Include',edk/'MdePkg/Include',edk/'MdePkg/Include/AArch64',edk/'MdeModulePkg/Include',edk/'EmbeddedPkg/Include']]+['-c',str(backend),'-o',str(arm)]
build=subprocess.run(armcmd,text=True,capture_output=True);armlog=a.output/'arm64-compile.log';armlog.write_text(build.stdout+build.stderr);build.check_returncode()
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
files={source,backend,Path(__file__).resolve(),backend.with_name('EspVariablePersistenceLib.inf'),crane/'Include/Library/VariablePersistenceLib.h'}
for dep in (hostdep,armdep):
 compiled={Path(f).resolve() for f in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1])}
 assert crane/'Include/Library/VariablePersistenceLib.h' in compiled
 assert edk/'MdeModulePkg/Include/Library/VariablePersistenceLib.h' not in compiled
 files.update(compiled)
(a.output/'artifacts.json').write_text(json.dumps({'scope':'Actual EFI ESP backend with modeled firmware/filesystem services plus actual ARM64 backend object','sources':{str(f):sha(f) for f in sorted(files)},'commands':[cmd,armcmd],'outputs':{str(f):sha(f) for f in (binary,arm,hostdep,armdep,hostlog,armlog,runlog)},'output':r.stdout},indent=2)+'\n')
