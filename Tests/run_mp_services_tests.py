#!/usr/bin/env python3
"""Actual blocking MP provider lifecycle + reloc-free ARM64 entry validation."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output.mkdir(parents=True,exist_ok=True);c=Path(__file__).resolve().parents[1];e=c.parents[1]/'MU_BASECORE'
d=c/'Driver/MpServicesCrDxe';inc=[e/'MdePkg/Include',e/'MdeModulePkg/Include',e/'ArmPkg/Include',e/'UefiCpuPkg/Include',d]
test=a.output/'mp-test';cmd=['clang','-std=c17','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-fsanitize=address,undefined','-fno-sanitize-recover=all',*[f'-I{x}' for x in inc],f'-I{e}/MdePkg/Include/X64',str(c/'Tests/MpServicesCrTest.c'),'-o',str(test)]
subprocess.run(cmd,check=True);runs=[]
modes=[['normal'],['weak-cache'],['fault-cache'],['el2'],['event'],['install'],['psci'],['fp'],['topology'],['foreign'],['late'],['off'],['on'],['retained-init']]
modes += [[kind,str(i)] for kind in ('alloc','attr') for i in range(1,16)]
for args in modes:
 r=subprocess.run([str(test),*args],check=True,capture_output=True,text=True,timeout=15);print(r.stdout,end='');runs.append({'args':args,'stdout':r.stdout})
obj=a.output/'entry.obj';subprocess.run(['clang','--target=aarch64-windows-msvc','-c','-x','assembler-with-cpp',f'-I{d}',str(d/'MpCrEntry.S'),'-o',str(obj)],check=True)
meta=subprocess.run(['llvm-readobj','--relocations','--symbols',str(obj)],check=True,capture_output=True,text=True).stdout
assert re.search(r'Relocations \[\s*\]',meta)
assert re.search(r'Name: MpCrTrampolineStart\s+Value: 0\s',meta)
assert re.search(r'Name: MpCrTrampolineEnd\s+Value: 4096\s',meta)
(a.output/'entry-metadata.txt').write_text(meta)
dis=subprocess.run(['llvm-objdump','-d',str(obj)],check=True,capture_output=True,text=True).stdout
(a.output/'entry-disassembly.txt').write_text(dis)
native=a.output/'provider.obj';subprocess.run(['clang','--target=aarch64-windows-msvc','-c','-ffreestanding','-fshort-wchar','-fno-builtin','-Wall','-Wextra','-Werror','-Wno-unused-parameter',*[f'-I{x}' for x in inc],f'-I{e}/MdePkg/Include/AArch64',str(d/'MpServicesCrDxe.c'),'-o',str(native)],check=True)
symbols=subprocess.run(['llvm-nm','--undefined-only',str(native)],check=True,capture_output=True,text=True).stdout
assert not re.search(r'\b(?:memcpy|memmove|memset|WriteBackDataCache)\b',symbols)
(a.output/'provider-undefined.txt').write_text(symbols)
inputs=[*d.glob('*'),c/'Tests/MpServicesCrTest.c',Path(__file__)]
(a.output/'artifacts.json').write_text(json.dumps({'runs':runs,'host_command':cmd,'assembly':'actual ARM64 COFF assembled and inspected; privileged instructions not executed by host fixture','inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs if x.is_file()},'outputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in (test,obj,native)}},indent=2)+'\n')
print('MP actual ARM64 provider +4096-byte zero-relocation copied entry PASS; no activation')
