#!/usr/bin/env python3
"""Actual SMEM RAM DXE/parser under ASan/UBSan with sparse GCD and MMU model."""
from pathlib import Path
import argparse, hashlib, json, subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser(description=__doc__);a.add_argument('--output',type=Path,required=True)
a.add_argument('--source',type=Path,default=p/'Driver/RamPartitionCrDxe/RamPartitionCrDxe.c')
a.add_argument('--expect-before-failure',action='store_true')
a.add_argument('--expect-policy-failure',action='store_true');args=a.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
includes=[p/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdePkg/Include/AArch64',root/'MU_BASECORE/MdeModulePkg/Include',root/'MU_BASECORE/UefiCpuPkg/Include',root/'Platforms/MUCommonCrPkg/Include',root/'Platforms/MUCommonCrPkg/Include']
sources=[p/'Tests/RamPartitionDxeTest.c',p/'Tests/RamPartitionSmmuTest.c',p/'Library/RamPartitionCrLib/RamPartition.c']
runs=[];dependencies=set()
for name,defines in [('debug',[]),('release',['-DMDEPKG_NDEBUG'])]:
 exe=out/name;cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DCR_RAM_DXE_SOURCE="'+str(args.source.resolve())+'"',*defines,*['-I'+str(x) for x in includes],*map(str,sources),'-o',str(exe)]
 subprocess.run(cmd,check=True)
 for source in sources:
  dep=out/(name+'-'+source.stem+'.d')
  dependency_command=['clang','-M','-MT',name,'-MF',str(dep),'-fshort-wchar','-DCR_RAM_DXE_SOURCE="'+str(args.source.resolve())+'"',*defines,*['-I'+str(x) for x in includes],str(source)]
  subprocess.run(dependency_command,check=True)
  import shlex
  dependencies.update(Path(x).resolve() for x in shlex.split(dep.read_text().replace('\\\n',' ').split(':',1)[1]))
 r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30);log=r.stdout+r.stderr;(out/(name+'.log')).write_text(log);print(log,end='')
 if args.expect_policy_failure:
  assert r.returncode!=0 and 'Attr==ExpectedAttributes' in log
  assert 'ERROR: AddressSanitizer' not in log and 'runtime error:' not in log
 elif args.expect_before_failure:
  assert r.returncode!=0 and 'PublishedDmaStatus==EFI_SUCCESS' in log
  assert 'ERROR: AddressSanitizer' not in log and 'runtime error:' not in log
 else:r.check_returncode()
 runs.append({'name':name,'command':cmd,'returncode':r.returncode,'expected_capability_failure':args.expect_before_failure,'expected_policy_failure':args.expect_policy_failure})
inputs=sorted(dependencies)+sources+[args.source.resolve(),p/'Tests/SmmuCrDxeTest.c',p/'Driver/SmmuCrDxe/SmmuCrDxe.c',p/'Include/Library/RamPartitionCrLib.h',Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'scope':__doc__,'runs':runs,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},'outputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(out.iterdir()) if x.is_file() and x.name!='artifacts.json'}},indent=2)+'\n')
