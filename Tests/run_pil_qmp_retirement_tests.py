#!/usr/bin/env python3
"""Actual PIL QMP retirement and affected native/NT lifecycle checks."""
import argparse,hashlib,json,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--core-source',type=Path);p.add_argument('--expect-before',action='store_true');a=p.parse_args()
cr=Path(__file__).resolve().parents[1];root=cr.parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
core=(a.core_source or cr/'Library/PILCrLib/Pil.c').resolve();inputs={};runs=[]
def sha(x):return hashlib.sha256(x.read_bytes()).hexdigest()
def run(name,sources,nt=False,smccc=False,extra=(),before=False):
 inc=[cr/'Include',cr/'Tests/PilMocks',cr/'Tests/KernelMocks'] if nt else [cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']
 flags=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all']+(['-D_KERNEL_MODE'] if nt else ['-fshort-wchar'])+(['-DPIL_TEST_SMCCC'] if smccc else [])+['-I'+str(x) for x in inc]+list(extra)
 for s in sources:
  d=subprocess.check_output(flags+['-MM',str(s)],text=True)
  for f in re.sub(r'\\\n',' ',d).split(':',1)[1].split():
   x=Path(f).resolve();inputs[str(x)]=sha(x)
 exe=out/name;cmd=flags+list(map(str,sources))+['-Wl,--gc-sections','-o',str(exe)];subprocess.run(cmd,check=True)
 r=subprocess.run([str(exe)]+(['before'] if before else []),text=True,capture_output=True,timeout=30)
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if before:
  if r.returncode==0 or 'S==CR_SUCCESS && DownWrites==0' not in r.stderr:raise RuntimeError('before did not reproduce consumer/provider contract failure')
 else:r.check_returncode()
 runs.append({'name':name,'command':cmd,'returncode':r.returncode,'binary':str(exe),'sha256':sha(exe),'output':r.stdout})
elf=cr/'Library/PILCrLib/PilElf.c'
for nt in (False,True):
 for helper in (False,True):
  tag=('nt' if nt else 'native')+('-smccc' if helper else '')
  units=[core,elf]+([cr/'Library/PILCrLib/PilSmccc.c'] if helper else [])
  run('qmp-'+tag,[cr/'Tests/PilQmpRetirementTest.c']+units,nt,helper,before=a.expect_before)
  if not a.expect_before:run('core-'+tag,[cr/'Tests/PilCoreTest.c']+units,nt,helper)
if not a.expect_before:
 run('native-adapter',[cr/'Tests/PilTest.c',elf,core,cr/'Library/PILCrLib/PilUefi.c'])
 run('native-ebs',[cr/'Tests/PilDxeLifecycleTest.c',cr/'Library/PILCrLib/PilClients.c',cr/'Library/PILCrLib/PilHandoffUefi.c',cr/'Library/RpmhArcVoteLib/RpmhArcVote.c',root/'MU_BASECORE/MdePkg/Library/BaseLib/CheckSum.c',elf])
 run('native-handoff',[cr/'Tests/PilHandoffUefiTest.c',cr/'Library/PILCrLib/PilHandoffUefi.c',cr/'Library/RpmhArcVoteLib/RpmhArcVote.c',root/'MU_BASECORE/MdePkg/Library/BaseLib/CheckSum.c',root/'MU_BASECORE/EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.c'],extra=['-Wno-unused-parameter'])
inputs[str(Path(__file__).resolve())]=sha(Path(__file__).resolve())
for f,h in inputs.items():
 if sha(Path(f))!=h:raise RuntimeError('source drift '+f)
outputs={str(f):sha(f) for f in out.iterdir() if f.is_file() and f.name!='artifacts.json'}
(out/'artifacts.json').write_text(json.dumps({'result':'PASS','expected_before':a.expect_before,'scope':'actual PIL core+affected native adapter/EBS/publisher; controlled peer/PAS/provider boundaries, no physical ACK claim','inputs':inputs,'outputs':outputs,'runs':runs},indent=2)+'\n')
print('PIL QMP retirement '+str(len(runs))+' native/NT actual-source modes PASS')
