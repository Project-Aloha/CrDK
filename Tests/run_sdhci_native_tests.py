#!/usr/bin/env python3
"""Actual native host/entry and shared PIO core with MMIO/lifetime faults."""
from pathlib import Path
import argparse,contextlib,hashlib,json,re,subprocess,tempfile
crane=Path(__file__).resolve().parents[1];root=crane.parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);a=p.parse_args()
ctx=contextlib.nullcontext(str(a.output.resolve())) if a.output else tempfile.TemporaryDirectory(prefix='crane-sdhci-native-')
with ctx as temporary:
 out=Path(temporary);out.mkdir(parents=True,exist_ok=True);inputs={};runs=[]
 sources=[crane/'Tests/SdhciNativeHostTest.c',crane/'Library/SdhciMsmLib/SdhciMsm.c']
 for name,extra in [('host-debug',[]),('host-release',['-DMDEPKG_NDEBUG'])]:
  exe=out/name
  opts=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',*extra,*['-I'+str(i) for i in [crane/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/EmbeddedPkg/Include']]]
  for s in sources:
   dep=subprocess.check_output(opts+['-MM',str(s)],text=True)
   for f in re.sub(r'\\\n',' ',dep).split(':',1)[1].split():
    x=Path(f).resolve();h=hashlib.sha256(x.read_bytes()).hexdigest()
    if str(x) in inputs and inputs[str(x)]!=h:raise RuntimeError('source drift '+str(x))
    inputs[str(x)]=h
  cmd=opts+['-ffunction-sections','-fdata-sections','-Wl,--gc-sections',*map(str,sources),'-o',str(exe)]
  subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],capture_output=True,text=True);(out/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
  runs.append({'mode':name,'command':cmd,'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
 inputs[str(Path(__file__).resolve())]=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
 for f,h in inputs.items():
  if hashlib.sha256(Path(f).read_bytes()).hexdigest()!=h:raise RuntimeError('source drift '+f)
 if a.output:(out/'artifacts.json').write_text(json.dumps({'result':'PASS','scope':'actual native entry/host/core; controlled MMIO/card/providers; no physical card claim','inputs':inputs,'runs':runs},indent=2)+'\n')
