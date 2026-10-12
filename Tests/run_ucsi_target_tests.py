#!/usr/bin/env python3
"""Actual UCSI target and DXE lifecycle DEBUG ASan/UBSan. MIT"""
import argparse,hashlib,json,shlex,subprocess
from pathlib import Path
crane=Path(__file__).resolve().parents[1];root=crane.parents[1]
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-missing',action='store_true');p.add_argument('--real-ipcc',action='store_true');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[crane/'Tests/Ucsi8350DxeLifecycleTest.c',crane/'Library/UcsiCrLib/PmicGlink.c',crane/'Library/UcsiCrLib/Qrtr.c',crane/'Library/UcsiCrLib/ChargerLog.c',crane/'Library/GpioPinLeaseLib/GpioPinLease.c']
if a.real_ipcc:sources[0]=crane/'Tests/Ucsi8350IpccLifecycleTest.c';sources.append(crane/'Library/UcsiCrLib/Ipcc.c')
flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [crane/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/EmbeddedPkg/Include']]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for src in sources:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(src)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
before={str(x.relative_to(root)):sha(x) for x in sorted(deps)}
exe=out/'DEBUG';cmd=['clang',*flags,'-Wl,--gc-sections',*map(str,sources),'-o',str(exe)]
r=subprocess.run(cmd,text=True,capture_output=True);(out/'compile.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(exe)],text=True,capture_output=True);(out/'test.log').write_text(r.stdout+r.stderr)
if a.expect_missing:assert r.returncode!=0 and 'Config->PinNumber' in r.stderr,r.stdout+r.stderr;print('RED: actual UCSI driver still uses SM8450 GPIO/IPCC on HDK8350')
else:assert r.returncode==0,r.stdout+r.stderr;print(r.stdout,end='')
assert all(sha(root/n)==h for n,h in before.items()),'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_missing else 'PASS','scope':'actualnativeUCSI DEBUG ASan/UBSan, modeledproviders; no firmware/deviceproof','inputs':before,'command':cmd,'executable_sha256':sha(exe)},indent=2)+'\n')
