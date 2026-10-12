#!/usr/bin/env python3
"""Actual SEC raw copy, HOBs, DXE FDT/Android import, UFS policy and DAL publication."""
import argparse, hashlib, json, shlex, subprocess, tarfile
from pathlib import Path

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--parser-source',type=Path)
p.add_argument('--expect-before',action='store_true')
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'artifacts.json').unlink(missing_ok=True)
c=Path(__file__).resolve().parents[1];root=c.parents[1];edk=root/'MU_BASECORE'
base=edk/'MdePkg/Library/BaseFdtLib';fdt=base/'libfdt/libfdt'
sources=[c/'Tests/UfsPowerPolicyIntegrationTest.c',
         a.parser_source.resolve() if a.parser_source else c/'Library/UfsPowerFdtCrLib/UfsPowerFdt.c',
         c/'Library/UfsPowerPolicyCrLib/UfsPowerPolicy.c',
         c/'Driver/CrDALDxe/BootFdt.c',c/'Library/AndroidBootCrLib/AndroidBoot.c',
         c/'Library/CrDalRegistryLib/CrDalRegistryLib.c',base/'FdtLib.c',*sorted(fdt.glob('*.c'))]
targets=[c/'Target/Sm8250/CrUfsTarget.c',c/'Target/Sm8450/CrUfsTarget.c']
blobs=[root/'Platforms/KonaCrPkg/Device/qcom-hdk8250/DeviceTreeBlob/Linux/linux-hdk8250.dtb',
       root/'Platforms/WaipioCrPkg/Device/qcom-hdk8450/DeviceTreeBlob/Android/android-hdk8450.dtb',
       root/'Platforms/KonaCrPkg/Device/qcom-hdk8250/DeviceTreeBlob/Android/android-hdk8250.dtb']
flags=['-std=c17','-D_GNU_SOURCE','-include','Uefi.h','-g','-O1','-fshort-wchar',
       '-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-sign-compare','-Wno-incompatible-pointer-types',
       '-fsanitize=address,undefined','-fno-sanitize-recover=all',
       '-I'+str(c/'Include'),'-I'+str(root/'Platforms/MUCommonCrPkg/Include'),
       '-I'+str(edk/'MdePkg/Include'),'-I'+str(edk/'MdePkg/Include/X64'),'-I'+str(edk/'ArmPkg/Include'),'-I'+str(edk/'EmbeddedPkg/Include'),
       '-idirafter',str(base),'-I'+str(fdt)]
sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
deps={Path(__file__).resolve(),*blobs}
for s in sources+targets:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
inputs={str(x):sha(x) for x in sorted(deps)}
(out/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
runs=[]
for mode in ['debug','release']:
 mf=flags+(['-O2','-DMDEPKG_NDEBUG'] if mode=='release' else [])
 objects=[]
 for soc,s in zip([8250,8450],targets):
  obj=out/f'{mode}-target{soc}.o';cmd=['clang',*mf,f'-DCrTargetGetUfsContext=CrTargetGetUfsContext{soc}','-c',str(s),'-o',str(obj)]
  subprocess.run(cmd,check=True);objects.append(obj)
 exe=out/mode;cmd=['clang',*mf,*map(str,sources),*map(str,objects),'-o',str(exe)]
 r=subprocess.run(cmd,capture_output=True,text=True);(out/(mode+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode: raise RuntimeError(r.stderr)
 for case in (['fixed-parent','ref-conflict'] if a.expect_before else ['normal','fixed-parent','ref-conflict']):
  run=[str(exe),str(blobs[0]),str(blobs[1]),case,str(blobs[2])];r=subprocess.run(run,capture_output=True,text=True,timeout=180)
  log=out/f'{mode}-{case}.log';log.write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
  if a.expect_before:
   if not r.returncode or f'hostile {case} status=0' not in r.stderr:raise RuntimeError('missing exact parser before failure')
  else:r.check_returncode()
  runs.append({'mode':mode,'case':case,'command':run,'returncode':r.returncode,'binary_sha256':sha(exe),'log_sha256':sha(log)})
drift=[x for x,h in inputs.items() if sha(Path(x))!=h]
if drift:raise RuntimeError('source drift '+repr(drift))
archive=out/'sources.tar.gz'
with tarfile.open(archive,'w:gz') as t:
 for name in sorted(inputs):t.add(name,arcname=name.lstrip('/'),recursive=False)
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','expected_before_failure':a.expect_before,'scope':__doc__,'limits':['HOB allocation, pool allocation, install services and physical memory/map are host boundaries; SEC capture, named HOB payloads, DXE import, MU FdtLib, Android parser, UFS policy and DAL publication are actual source','Logical original ranges use low fixed mmap addresses to avoid ASan shadow; no native firmware or hardware validation'],'source_archive':str(archive),'source_archive_sha256':sha(archive),'inputs':inputs,'runs':runs},indent=2)+'\n')
print('receipt',sha(out/'artifacts.json'))
