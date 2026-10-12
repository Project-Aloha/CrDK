#!/usr/bin/env python3
"""Actual FDT/SPMI/PDC/OSKAL/shared Buttons+CC native composition under sanitizers."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess,tempfile
canonical=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--crane',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--workspace',type=Path,default=canonical.parents[1]);p.add_argument('--irq-crane',type=Path);p.add_argument('--output',type=Path);p.add_argument('--config',choices=['debug','release','both'],default='both');p.add_argument('--modes',type=int,nargs='+');p.add_argument('--spmi-dxe-source',type=Path);a=p.parse_args()
C=a.crane.resolve();W=a.workspace.resolve();canonical=W/'Platforms/CranePkg';I=(a.irq_crane or C).resolve()
Temp=tempfile.TemporaryDirectory(prefix='crane-shared-buttons-')if not a.output else None;O=(a.output or Path(Temp.name)).resolve();O.mkdir(parents=True,exist_ok=True)
def source(n):return C/n if(C/n).is_file()else canonical/n
base=W/'MU_BASECORE/MdePkg/Library/BaseFdtLib';fdt=base/'libfdt/libfdt';blob=W/'Platforms/KonaCrPkg/Device/qcom-hdk8245/DeviceTreeBlob/Source/hdk8245-runtime.dtb'
sources=[source(n)for n in ['Tests/ButtonsCrTest.c','Driver/SpmiCrDxe/Board.c','Library/SpmiSharedCrLib/SpmiShared.c','Library/PmicCcCrLib/PmicCc.c','Library/ButtonsCrLib/Buttons.c','Library/PmicGpioLib/pmic_gpio.c','Library/PdcLib/pdc.c']]+[I/'Library/OskInterruptLib/oskinterrupt.c',base/'FdtLib.c',*sorted(fdt.glob('*.c')),*[source('Library/SpmiLib/'+s.name)for s in sorted((canonical/'Library/SpmiLib').glob('*.c'))]]
incs=[I/'Include',C/'Include',canonical/'Include',canonical/'Library/SpmiLib',W/'Platforms/MUCommonCrPkg/Include',W/'Common/MU_OEM_SAMPLE/OemPkg/Include',W/'MU_BASECORE/EmbeddedPkg/Include',W/'MU_BASECORE/MdePkg/Include',W/'MU_BASECORE/MdePkg/Include/X64',fdt]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();commands=[];inputs={str(blob):sha(blob)};results=[]
for config in (['debug','release']if a.config=='both'else[a.config]):
 flags=['clang','-idirafter',str(base),'-std=c17','-D_GNU_SOURCE','-include','Uefi.h','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-Wno-incompatible-pointer-types','-Wno-sign-compare','-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(i)for i in incs]]
 if config=='release':flags+=['-DMDEPKG_NDEBUG']
 if a.spmi_dxe_source:flags+=['-DCR_SPMI_DXE_SOURCE="'+str(a.spmi_dxe_source.resolve())+'"']
 for src in sources:
  dep=O/(config+'-'+src.stem+'.d');cmd=[*flags,'-M','-MF',str(dep),str(src)];commands.append(cmd);subprocess.run(cmd,check=True)
  for n in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]):
   x=Path(n).resolve();h=sha(x);assert str(x)not in inputs or inputs[str(x)]==h;inputs[str(x)]=h
 binary=O/config;cmd=[*flags,*map(str,sources),'-o',str(binary)];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True);(O/(config+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
 for mode in (a.modes if a.modes is not None else range(33)):
  cmd=[str(binary),str(mode),str(blob)];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,timeout=20);(O/(config+'-'+str(mode)+'.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr;results.append({'config':config,'mode':mode,'passed':True})
assert all(sha(Path(k))==v for k,v in inputs.items())
(O/'artifacts.json').write_text(json.dumps({'scope':__doc__,'commands':commands,'inputs':inputs,'results':results,'outputs':{str(x):sha(x)for x in O.iterdir()if x.is_file()and x.name!='artifacts.json'}},indent=2)+'\n');print('PASS',O)
