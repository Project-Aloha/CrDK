#!/usr/bin/env python3
"""Compile actual HDK USB lifecycle and each target with ASan/UBSan."""
import argparse,hashlib,json,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path);p.add_argument('--target',choices=['8250','8350','both'],default='both');p.add_argument('--dxe',action='store_true');p.add_argument('--cases',nargs='+',help='focused actual fixture cases, for immutable old-source regression checks');p.add_argument('--primary',action='store_true',help='actual HDK8245 USB0, PM8150B witness and PMIC native callback compositions');a=p.parse_args()
cr=Path(__file__).resolve().parents[1];root=cr.parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
flags += ['-I'+str(x) for x in [cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']]
def run(label,sources,extra,cases,scope):
 current_flags=flags+extra
 deps={Path(__file__).resolve()}
 for src in sources:
  r=subprocess.run(['clang',*current_flags,'-MM',str(src)],text=True,capture_output=True)
  if r.returncode: print(r.stderr,end='');raise SystemExit(r.returncode)
  deps.update(Path(n).resolve() for n in shlex.split(r.stdout.replace('\\\n',' ').split(':',1)[1]))
 hashes={str(x.relative_to(root)):hashlib.sha256(x.read_bytes()).hexdigest() for x in sorted(deps)}
 cmd=['clang',*current_flags,*map(str,sources),'-o',str(out/('usb-'+label))]
 r=subprocess.run(cmd,text=True,capture_output=True);(out/(label+'-compile.log')).write_text(r.stdout+r.stderr)
 if r.returncode: print(r.stderr,end='');raise SystemExit(r.returncode)
 results={}
 for case in (a.cases or cases):
  r=subprocess.run([str(out/('usb-'+label))]+([] if case=='all' else [case]),text=True,capture_output=True)
  (out/(label+'-'+case+'-test.log')).write_text(r.stdout+r.stderr);results[case]=r.returncode
  print(r.stdout+r.stderr,end='')
  if r.returncode:break
 assert all(hashlib.sha256((root/n).read_bytes()).hexdigest()==h for n,h in hashes.items())
 (out/(label+'-receipt.json')).write_text(json.dumps({'scope':scope,'command':cmd,'inputs':hashes,'cases':results,'exit':r.returncode,'hardware_claim':False},indent=2)+'\n')
 if r.returncode:raise SystemExit(r.returncode)
if a.primary:
 run('primary8250',[cr/'Tests/HdkUsbDxeTest.c',cr/'Library/HdkUsbCrLib/HdkUsb.c',cr/'Target/Sm8250/CrUsbTarget.c',cr/'Library/PmicCcCrLib/PmicCc.c'],['-DCR_HDK_USB_PRIMARY','-DCR_HDK_PRIMARY_TEST'],['all'],'actual PM8150B/native primary USB2 non-PD source witness; modeled firmware/provider/MMIO boundaries')
 run('pmic-witness',[cr/'Tests/PmicCcHostWitnessTest.c',cr/'Library/PmicCcCrLib/PmicCc.c'],[],['all'],'actual PM8150B sample/publication core; modeled shared SPMI boundary')
 run('pmic-dxe',[cr/'Tests/PmicCcDxeTest.c',cr/'Library/PmicCcCrLib/PmicCc.c'],[],['all','entry','unload','exit'],'actual PM8150B native observer/core callbacks; modeled firmware/shared SPMI boundaries')
else:
 for target in (['8250','8350'] if a.target=='both' else [a.target]):
  run(target,[cr/'Tests'/('HdkUsbDxeTest.c' if a.dxe else 'HdkUsbCrTest.c'),(a.source or cr/'Library/HdkUsbCrLib/HdkUsb.c').resolve(),cr/'Target'/('Sm'+target)/'CrUsbTarget.c'],[],['all','entry','unload','exit','provider-exit'] if a.dxe else ['all'],'actual native fixed-host USB2 adapter/core and board resources; modeled hardware boundary')
