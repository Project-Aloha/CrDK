#!/usr/bin/env python3
"""Actual raw SEC transport, arena ownership and hostile-map tests. MIT."""
import argparse, hashlib, json, shlex, shutil, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--before-source',type=Path);a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);receipt=out/'artifacts.json';receipt.unlink(missing_ok=True)
c=Path(__file__).resolve().parents[1];root=c.parents[1];edk=root/'MU_BASECORE'
source=c/'Tests/BootDataCaptureTest.c';producer=c/'Library/BootModeCaptureCrLib/BootModeCaptureCrLib.c'
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
compiler=Path(shutil.which('clang')).resolve()
incs=['-I'+str(c/'Include'),'-I'+str(root/'Platforms/MUCommonCrPkg/Include'),'-I'+str(edk/'MdePkg/Include'),'-I'+str(edk/'ArmPkg/Include'),'-I'+str(edk/'EmbeddedPkg/Include')]
flags=['-std=c17','-g','-O1','-fshort-wchar','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',*incs,'-I'+str(edk/'MdePkg/Include/X64')]
arm=['-target','aarch64-none-elf','-Oz','-std=c17','-ffreestanding','-fshort-wchar','-mstrict-align','-fno-stack-protector','-fstack-usage','-Wall','-Wextra','-Werror',*incs,'-I'+str(edk/'MdePkg/Include/AArch64')]
def deps(text):return {Path(x).resolve() for x in shlex.split(text.replace('\\\n',' ').split(':',1)[1])}
probe=c/'Tests/BootDataNameBoundTest.c'
inputs={Path(__file__).resolve(),probe,c/'Library/BootModeCaptureCrLib/BootModeCaptureCrLib.inf'}
if a.before_source:inputs.add(a.before_source.resolve())
for args,src in [(flags,source),(arm,producer)]:inputs.update(deps(subprocess.check_output([str(compiler),*args,'-MM',str(src)],text=True)))
held={str(f):sha(f) for f in sorted(inputs)}
def run(cmd,name):
 r=subprocess.run(cmd,capture_output=True,text=True);(out/name).write_text(r.stdout+r.stderr)
 if r.returncode:raise RuntimeError(r.stdout+r.stderr)
 return r
builds=[]
for mode in ['debug','release']:
 exe=out/mode;dep=out/(mode+'.d')
 cmd=[str(compiler),*flags,*(['-DMDEPKG_NDEBUG','-O2'] if mode=='release' else []),'-MMD','-MF',str(dep),str(source),'-o',str(exe)]
 run(cmd,mode+'-compile.log');r=run([str(exe)],mode+'.log');print(mode,r.stdout.strip())
 assert deps(dep.read_text())<=inputs
 builds.append({'command':cmd,'binary_sha256':sha(exe),'dependency_sha256':sha(dep),'log_sha256':sha(out/(mode+'.log'))})
obj=out/'BootModeCapture.o';cmd=[str(compiler),*arm,'-MMD','-MF',str(out/'arm64.d'),'-c',str(producer),'-o',str(obj)]
run(cmd,'arm64.log');assert deps((out/'arm64.d').read_text())<=inputs
probes=[]
for kind,original in [('after',producer)]+([('before',a.before_source.resolve())] if a.before_source else []):
 text=original.read_text()
 needle='Fdt[Strings + NameEnd] != 0' if kind=='before' else 'P[I] != (UINT8)Name[I]'
 assert text.count(needle)==1,'Property-name instrumentation point changed'
 instrumented=out/(kind+'-instrumented.c')
 instrumented.write_text(text.replace(needle,'(++NameReads, '+needle+')'))
 exe=out/(kind+'-name-bound')
 command=[str(compiler),'-std=c17','-O2','-fshort-wchar','-ffunction-sections','-fdata-sections',*incs,
          '-I'+str(edk/'MdePkg/Include/X64'),'-DBOOT_DATA_PROBE_SOURCE="'+str(instrumented)+'"',
          str(probe),'-Wl,--gc-sections','-o',str(exe)]
 run(command,kind+'-name-compile.log')
 result=subprocess.run([str(exe)],capture_output=True,text=True)
 (out/(kind+'-name.log')).write_text(result.stdout+result.stderr)
 assert result.returncode==(1 if kind=='before' else 0),result.stdout+result.stderr
 print(kind,result.stdout.strip())
 probes.append({'kind':kind,'source_sha256':sha(original),'command':command,'returncode':result.returncode,
                'binary_sha256':sha(exe),'output':result.stdout,
                'instrumentation':'Only count each inspected property-name byte in copied actual transport TU'})
assert all(sha(Path(f))==h for f,h in held.items())
receipt.write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':held,'compiler':str(compiler),'compiler_sha256':sha(compiler),'builds':builds,'name_bound_probes':probes,'arm64_command':cmd,'arm64_object_sha256':sha(obj),'stack_usage':(out/'BootModeCapture.su').read_text()},indent=2)+'\n');print('receipt',sha(receipt))
