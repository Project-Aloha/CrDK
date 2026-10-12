#!/usr/bin/env python3
"""Actual HDK GOP, native parsers and MU FrameBufferBlt with hardware/services isolated."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--case',action='append')
p.add_argument('--board',type=int,action='append',choices=[8245,8350])
a=p.parse_args();root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg'
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
seed=root/'.tmp/waipio-continuation/hdk8245-panel/boot-observe/validation-v2/BootObserveReference.c'
sources=[pkg/'Tests/HdkBootDisplayTest.c',seed,
 pkg/'Library/MdssBoot8250CrLib/Observe.c',pkg/'Library/MdssBoot8250CrLib/Io.c',
 pkg/'Library/Hdk8350BootDisplayCrLib/Hdk8350BootDisplay.c',
 root/'MU_BASECORE/MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.c']
flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror',
 '-Wno-misleading-indentation','-Wno-sign-compare','-fshort-wchar',
 '-fsanitize=address,undefined','-fno-sanitize-recover=all',
 '-ffunction-sections','-fdata-sections']
for q in [pkg/'Include',root/'MU_BASECORE/MdePkg/Include',
 root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include',
 root/'Platforms/MUCommonCrPkg/Include']:flags += ['-I'+str(q)]
inputs={};commands=[];results=[]
def pin(q):
 q=q.resolve();data=q.read_bytes();h=hashlib.sha256(data).hexdigest()
 if str(q) in inputs:assert inputs[str(q)]==h,q
 else:inputs[str(q)]=h
def run(cmd,log):
 commands.append(list(map(str,cmd)));r=subprocess.run(cmd,text=True,capture_output=True,timeout=120)
 (out/log).write_text(r.stdout+r.stderr);return r
for q in [*sources,Path(__file__),pkg/'Driver/HdkBootDisplayCrDxe/HdkBootDisplayCrDxe.c']:pin(q)
cases=a.case or ['success','packing','padding','route','gate','smmu','smmu-pending','translated','physical',
 'reservation','writeback','cpu-map','readonly','partial-install','no-progress','allocation','install','reentry','retained','bounds','move','ebs']
for board in a.board or [8245,8350]:
 objects=[]
 for i,q in enumerate(sources):
  dep=out/f'{board}-{i}.d';obj=out/f'{board}-{i}.o';bflags=[*flags,f'-DHDK_TEST_BOARD={board}']
  r=run([*bflags,'-MM','-MF',dep,q],f'{board}-{i}-deps.log')
  if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
  for name in dep.read_text().split(':',1)[1].replace('\\\n','').split():pin(Path(name))
  extra=['-fno-sanitize=shift-base'] if q==seed else []
  r=run([*bflags,*extra,'-c',q,'-o',obj],f'{board}-{i}-compile.log')
  if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
  objects.append(obj)
 exe=out/f'gop-{board}'
 r=run(['clang','-fsanitize=address,undefined','-Wl,--gc-sections',*objects,'-o',exe],f'{board}-link.log')
 if r.returncode:print(r.stderr);raise SystemExit(r.returncode)
 for case in cases:
  r=run([exe,case],f'{board}-{case}.log');print(r.stdout+r.stderr,end='')
  results.append({'board':board,'case':case,'exit_code':r.returncode})
 for q,h in inputs.items():assert hashlib.sha256(Path(q).read_bytes()).hexdigest()==h,q
 results.append({'board':board,'executable_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
(out/'receipt.json').write_text(json.dumps({'scope':__doc__,'inputs':inputs,'commands':commands,
 'results':results,'status':'PASS' if all(x.get('exit_code',0)==0 for x in results) else 'FAIL'},indent=2)+'\n')
raise SystemExit(0 if all(x.get('exit_code',0)==0 for x in results) else 1)
