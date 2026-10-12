#!/usr/bin/env python3
"""Actual common MDSS resources + retirement + typed SMMU under ASan/UBSan."""
import argparse, hashlib, json, shlex, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
pkg=Path(__file__).resolve().parents[1];root=pkg.parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[pkg/n for n in ['Tests/MdpNativeResourcesTest.c','Library/SmmuLib/smmu.c','Library/SmmuLib/SmmuDisplay.c','Library/SmmuLib/SmmuDisplayOwnership.c','Library/MdssCrLib/MdssRetire.c','Library/MdssCrLib/MdssHardware.c','Library/ClockExternalLib/ClockExternalPlan.c']]
flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-fno-pie']
flags+=['-I'+str(x) for x in [pkg/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for s in sources:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
before={str(x.relative_to(root)):sha(x) for x in sorted(deps)};exe=out/'DEBUG'
cmd=['clang',*flags,'-no-pie','-Wl,--gc-sections',*map(str,sources),'-o',str(exe)]
r=subprocess.run(cmd,text=True,capture_output=True);(out/'compile.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=120);(out/'test.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr;print(r.stdout,end='')
assert all(sha(root/n)==h for n,h in before.items()),'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'PASS','scope':__doc__+'; modeled suppliers/MMIO, no hardware claim','inputs':before,'command':cmd,'executable_sha256':sha(exe)},indent=2)+'\n')
