#!/usr/bin/env python3
"""Actual DXE raw-FDT importer, MU libfdt, Android mode and UFS parsers."""
from pathlib import Path
import argparse,datetime,hashlib,json,shlex,subprocess,tarfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);assert not(out/'artifacts.json').exists()
c=Path(__file__).resolve().parents[1];root=c.parents[1];edk=root/'MU_BASECORE';base=edk/'MdePkg/Library/BaseFdtLib';libfdt=base/'libfdt/libfdt'
sources=[c/'Tests/BootFdtImportTest.c',c/'Driver/CrDALDxe/BootFdt.c',c/'Library/AndroidBootCrLib/AndroidBoot.c',c/'Library/UfsPowerFdtCrLib/UfsPowerFdt.c',base/'FdtLib.c',*sorted(libfdt.glob('*.c'))]
blobs=[root/'Platforms/KonaCrPkg/Device/qcom-hdk8250/DeviceTreeBlob/Linux/linux-hdk8250.dtb',root/'Platforms/WaipioCrPkg/Device/qcom-hdk8450/DeviceTreeBlob/Android/android-hdk8450.dtb',root/'Platforms/KonaCrPkg/Device/qcom-hdk8250/DeviceTreeBlob/Android/android-hdk8250.dtb']
flags=['-std=c17','-D_GNU_SOURCE','-include','Uefi.h','-g','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-unused-function','-Wno-sign-compare','-Wno-incompatible-pointer-types','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include'),'-I'+str(edk/'MdePkg/Include'),'-I'+str(edk/'MdePkg/Include/X64'),'-idirafter',str(base),'-I'+str(libfdt)]
sha=lambda f:hashlib.sha256(Path(f).read_bytes()).hexdigest();deps={Path(__file__).resolve(),*blobs};commands=[];runs=[]
for src in sources:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(src)],text=True).replace('\\\n',' ');deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
inputs={str(f):sha(f) for f in sorted(deps)}
for mode,opt in [('debug','-O1'),('release','-O2')]:
 exe=out/mode;cmd=['clang',*flags,opt,*(['-DMDEPKG_NDEBUG'] if mode=='release' else []),*map(str,sources),'-o',str(exe)];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True);(out/(mode+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
 cmd=[str(exe),*map(str,blobs)];commands.append(cmd);r=subprocess.run(cmd,capture_output=True,text=True,timeout=60);log=out/(mode+'.log');log.write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();runs.append({'mode':mode,'binary':str(exe),'binary_sha256':sha(exe),'log':str(log),'log_sha256':sha(log)})
for f,h in inputs.items():assert sha(f)==h,('drift',f)
archive=out/'sources.tar.gz'
with tarfile.open(archive,'w:gz') as t:
 for f in sorted(inputs):t.add(f,arcname=f.lstrip('/'),recursive=False)
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','checked_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'inputs':inputs,'commands':commands,'runs':runs,'archive':str(archive),'archive_sha256':sha(archive),'scope':__doc__,'limits':['OS allocation and HOB construction modeled; actual SEC producer/DAL/charger composition and native firmware build are separate root checks.']},indent=2)+'\n');print('receipt',sha(out/'artifacts.json'))
