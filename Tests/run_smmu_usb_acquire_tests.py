#!/usr/bin/env python3
"""Actual stopped USB SMMU DXE/core DEBUG ASan/UBSan contract controls.
SPDX-License-Identifier: MIT"""
import argparse,hashlib,json,shlex,subprocess
from pathlib import Path
crane=Path(__file__).resolve().parents[1];root=crane.parents[1];edk=root/'MU_BASECORE'
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-missing',action='store_true');a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[crane/'Library/SmmuLib/smmu.c',crane/'Library/SmmuLib/SmmuDisplay.c',crane/'Library/SmmuLib/SmmuDisplayOwnership.c',crane/'Tests/SmmuUsbDxeAcquireTest.c']
flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [crane/'Include',edk/'MdePkg/Include',edk/'MdePkg/Include/X64',edk/'MdeModulePkg/Include']]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for src in sources:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(src)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
before={str(x.relative_to(root)):sha(x) for x in sorted(deps)}
exe=out/'DEBUG';cmd=['clang',*flags,'-Wl,--gc-sections',*map(str,sources),'-o',str(exe)]
r=subprocess.run(cmd,text=True,capture_output=True);(out/'compile.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(exe)],text=True,capture_output=True);(out/'test.log').write_text(r.stdout+r.stderr)
if a.expect_missing:
 assert r.returncode!=0 and 'Acquire()==EFI_SUCCESS' in r.stderr,r.stdout+r.stderr
 print('RED: ordinary Attach refuses the actual inherited USB route')
else:assert r.returncode==0,r.stdout+r.stderr;print(r.stdout,end='')
assert all(sha(root/n)==h for n,h in before.items()),'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_missing else 'PASS','scope':'actual DXE+SMMUcore+bothtargets DEBUG ASan/UBSan; modeledMMIO/allocator, no hardware proof','inputs':before,'command':cmd,'executable_sha256':sha(exe)},indent=2)+'\n')
