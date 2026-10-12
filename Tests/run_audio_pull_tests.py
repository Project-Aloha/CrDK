#!/usr/bin/env python3
"""Actual typed compiler/graph/Pull retained runtime and controlled remote peer; no hardware claims."""
import argparse,hashlib,json,shlex,subprocess,tarfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--acdb',type=Path,default=Path(__file__).resolve().parent/'AudioPull/Fixtures/connected.acdb');p.add_argument('--request',type=Path,default=Path(__file__).resolve().parent/'AudioPull/Fixtures/connected.request.bin');a=p.parse_args()
t=Path(__file__).resolve().parent/'AudioPull';base=t.parent;o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
cr=Path(__file__).resolve().parents[1];r=cr.parents[1];stage=cr
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
fixture=a.acdb.resolve();request=a.request.resolve()
sources=[t/'PullRuntimeTest.c',t/'AudioGraphPeer.c',stage/'Library/AudioGraphCrLib/AudioGraph.c',stage/'Library/AudioPullCrLib/Pull.c',cr/'Library/AudioPcmCrLib/AudioPcm.c',stage/'Library/AudioCalCrLib/AudioCal.c',cr/'Library/AcdbCrLib/Acdb.c',cr/'Library/AudioGprCrLib/AudioGpr.c',cr/'Library/UcsiCrLib/Glink.c']
(o/'Glink.tmh').write_text('/* private host trace shim */\n')
inputs={Path(__file__).resolve(),fixture,request,*sources};runs=[]
for nt in (False,True):
 flags=['-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(stage/'Include')]
 if nt:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(cr/'Tests/KernelMocks'),'-I'+str(o)]
 else:flags+=['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
 flags+=['-I'+str(cr/'Include')]
 for s in sources:
  dep=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True);inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n','').split(':',1)[1]))
 before={str(f):sha(f) for f in sorted(inputs)}
 exe=o/('graph-'+('nt' if nt else 'efi'));cmd=['clang',*flags,*map(str,sources),'-o',str(exe)]
 result=subprocess.run(cmd,capture_output=True,text=True);(o/(exe.name+'-build.log')).write_text(result.stdout+result.stderr)
 if result.returncode:print(result.stdout+result.stderr);result.check_returncode()
 run=[str(exe),str(fixture),str(request)];result=subprocess.run(run,capture_output=True,text=True,timeout=120);(o/(exe.name+'.log')).write_text(result.stdout+result.stderr);print(result.stdout+result.stderr,end='')
 assert before=={str(f):sha(f) for f in sorted(inputs)}
 runs.append({'nt':nt,'command':cmd,'run':run,'status':result.returncode,'binary':sha(exe),'inputs':before})
 if result.returncode:(o/'failed.json').write_text(json.dumps(runs,indent=2)+'\n');result.check_returncode()
with tarfile.open(o/'inputs.tar.gz','w:gz') as z:
 for f in sorted(inputs):z.add(f,arcname=str(f).lstrip('/'),recursive=False)
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'Actual typed cal/graph/Pull/PCM/GPR/GLINK source; explicit synthetic data and controlled remote peer; no calibration or hardware grant','inputs':{str(f):sha(f) for f in sorted(inputs)},'runs':runs,'archive_sha256':sha(o/'inputs.tar.gz')},indent=2)+'\n')
