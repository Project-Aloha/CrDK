#!/usr/bin/env python3
"""Run actual HDK8245/8350 PCIe targets with native core/host power logic."""
import argparse,hashlib,json,subprocess,tempfile
from pathlib import Path
cr=Path(__file__).resolve().parents[1];root=cr.parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path);a=p.parse_args()
def run(out):
 out.mkdir(parents=True,exist_ok=True);results=[]
 for soc,n in [('8250',3),('8350',2)]:
  sources=[cr/'Tests/HdkPcieTest.c',cr/f'Target/Sm{soc}/CrPcieTarget.c',cr/f'Target/Sm{soc}/CrSmmuTarget.c']
  flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections',f'-DHDK_PORTS={n}']
  for inc in [cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']:flags+=['-I'+str(inc)]
  exe=out/soc
  for label,cmd in [('compile',['clang',*flags,*map(str,sources),'-o',str(exe)]),('run',[str(exe)])]:
   r=subprocess.run(cmd,text=True,capture_output=True);(out/f'{soc}-{label}.log').write_text(r.stdout+r.stderr)
   assert r.returncode==0,r.stdout+r.stderr
   if label=='run':print(r.stdout,end='')
  results.append({'soc':soc,'ports':n,'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'status':'PASS'})
  diag_sources=[cr/'Tests/HdkPcieDiagnosticsTest.c',cr/f'Target/Sm{soc}/CrPcieTarget.c']
  diag_exe=out/f'{soc}-diagnostics'
  for label,cmd in [('compile',['clang',*flags,*map(str,diag_sources),'-o',str(diag_exe)]),('run',[str(diag_exe)])]:
   r=subprocess.run(cmd,text=True,capture_output=True);(out/f'{soc}-diagnostics-{label}.log').write_text(r.stdout+r.stderr)
   assert r.returncode==0,r.stdout+r.stderr
   if label=='run':print(r.stdout,end='')
  results.append({'soc':soc,'ports':n,'case':'diagnostics','sha256':hashlib.sha256(diag_exe.read_bytes()).hexdigest(),'status':'PASS'})
 (out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
if a.output:run(a.output.resolve())
else:
 with tempfile.TemporaryDirectory(prefix='hdk-pcie-') as d:run(Path(d))
