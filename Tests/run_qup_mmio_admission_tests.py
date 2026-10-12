#!/usr/bin/env python3
"""Actual QUP lease + I2C15 reject missing CPU MMIO mappings before effects."""
import argparse,hashlib,json,shlex,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-before',action='store_true');a=p.parse_args()
pkg=Path(__file__).resolve().parents[1];root=pkg.parents[1];o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
s=pkg/'Tests/QupMmioAdmissionTest.c';sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
flags=['-std=c17','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [pkg/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']]
raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
deps={Path(__file__).resolve(),*(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))};before={str(x.relative_to(root)):sha(x) for x in sorted(deps)}
exe=o/'DEBUG';cmd=['clang',*flags,'-Wl,--gc-sections',str(s),'-o',str(exe)]
r=subprocess.run(cmd,text=True,capture_output=True);(o/'compile.log').write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
r=subprocess.run([str(exe)],text=True,capture_output=True);(o/'test.log').write_text(r.stdout+r.stderr)
if a.expect_before:assert r.returncode!=0 and 'QupAcquireSe' in r.stderr,r.stdout+r.stderr;print('EXPECTED RED: missing CPU MMIO mapping granted QUP lease')
else:assert r.returncode==0,r.stdout+r.stderr;print(r.stdout,end='')
assert all(sha(root/n)==h for n,h in before.items()),'source drift'
(o/'receipt.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_before else 'PASS','scope':__doc__+'; modeled GCD/MMIO/providers only','inputs':before,'command':cmd,'executable_sha256':sha(exe)},indent=2)+'\n')
