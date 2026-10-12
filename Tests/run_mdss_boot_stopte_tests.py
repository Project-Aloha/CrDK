#!/usr/bin/env python3
"""Compare actual complete boot owner's successful StopTe MMIO with Android originals."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[3];pkg=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
base=root/'.tmp/waipio-continuation/hdk8245-panel'
helper=base/'boot-retirement/retire-peer/stopte-oracle';seed=base/'boot-observe/validation-v2/BootObserveReference.c'
sources=[pkg/'Tests/MdssBootStopTeTest.c',seed,helper/'AndroidStopTeOracle.c',*[pkg/'Library/MdssBoot8250CrLib'/x for x in ('Io.c','Observe.c','Retire.c')]]
digest=lambda q:hashlib.sha256(q.read_bytes()).hexdigest();frozen={};commands=[]
def pin(q):
 q=q.resolve();d=digest(q);assert q not in frozen or frozen[q]==d,q;frozen[q]=d
for q in [*sources,Path(__file__),helper/'source-manifest.json',helper/'test-receipt.json',base/'boot-observe/validation-v2/artifacts.json']:pin(q)
def run(cmd,log=None):
 commands.append(list(map(str,cmd)));r=subprocess.run(cmd,text=True,capture_output=True)
 if log:(out/log).write_text(r.stdout+r.stderr)
 if r.returncode:print(r.stdout+r.stderr);r.check_returncode()
 return r
for nt in (False,True):
 mode='nt' if nt else 'native';objs=[]
 flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-sign-compare','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(pkg/'Include'),'-I'+str(helper)]
 flags+=['-D_KERNEL_MODE','-I'+str(pkg/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 for i,q in enumerate(sources):
  dep=out/f'{mode}-{i}.d';obj=out/f'{mode}-{i}.o'
  run([*flags,'-MM','-MF',dep,q])
  for x in dep.read_text().split(':',1)[1].replace('\\\n','').split():pin(Path(x))
  extra=['-fno-sanitize=shift-base'] if q==seed else ['-Wno-unused-but-set-variable'] if q==helper/'AndroidStopTeOracle.c' else []
  run([*flags,*extra,'-c',q,'-o',obj],f'{mode}-{i}-compile.log');objs.append(obj)
 exe=out/mode;run(['clang','-fsanitize=address,undefined',*objs,'-o',exe]);r=run([exe],f'{mode}.log');print(mode+': '+r.stdout.strip())
for q,d in frozen.items():assert digest(q)==d,q
(out/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'commands':commands,'inputs':{str(q):d for q,d in sorted(frozen.items())},'artifacts':{q.name:digest(q) for q in out.iterdir() if q.is_file() and q.name!='artifacts.json'}},indent=2)+'\n')
