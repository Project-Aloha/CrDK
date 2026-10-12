#!/usr/bin/env python3
"""Actual v5 PIO FIFO selection regression in native and NT status modes."""
import argparse,hashlib,json,subprocess,re
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--core-source',type=Path);p.add_argument('--expect-before',action='store_true');a=p.parse_args()
cr=Path(__file__).resolve().parents[1];root=cr.parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
core=(a.core_source or cr/'Library/SdhciMsmLib/SdhciMsm.c').resolve();runs=[];inputs={}
for nt in [False,True]:
 name='nt' if nt else 'native';exe=out/name
 inc=[cr/'Include',cr/'Tests/KernelMocks'] if nt else [cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']
 cmd=['clang','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']+(['-D_KERNEL_MODE'] if nt else ['-fshort-wchar'])+['-I'+str(x) for x in inc]+[str(cr/'Tests/SdhciPioProfileTest.c'),str(core),'-o',str(exe)]
 for s in [cr/'Tests/SdhciPioProfileTest.c',core]:
  dep=subprocess.check_output(cmd[:cmd.index(str(cr/'Tests/SdhciPioProfileTest.c'))]+['-MM',str(s)],text=True)
  for f in re.sub(r'\\\n',' ',dep).split(':',1)[1].split():
   x=Path(f).resolve();inputs[str(x)]=hashlib.sha256(x.read_bytes()).hexdigest()
 subprocess.run(cmd,check=True);r=subprocess.run([str(exe)]+(['inherited'] if a.expect_before else []),text=True,capture_output=True)
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if a.expect_before:
  if r.returncode==0 or '!(M.Value&MASK)' not in r.stderr:raise RuntimeError('before must fail inherited FIFO assertion')
 else:r.check_returncode()
 runs.append({'mode':name,'command':cmd,'returncode':r.returncode,'stdout':r.stdout,'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
inputs[str(Path(__file__).resolve())]=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
for f,h in inputs.items():
 if hashlib.sha256(Path(f).read_bytes()).hexdigest()!=h:raise RuntimeError('source drift '+f)
(out/'artifacts.json').write_text(json.dumps({'result':'PASS','expected_before_failure':a.expect_before,'scope':'actual portable core with controlled vendor3 IO; no reset/hardware grant','inputs':inputs,'runs':runs},indent=2)+'\n')
print('FIFO profile '+('authentic before rejection' if a.expect_before else 'current native/NT')+' PASS')
