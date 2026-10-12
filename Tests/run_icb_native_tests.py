#!/usr/bin/env python3
"""DEBUG native ICC lifecycle/retained-error regression with input receipt."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
pkg=Path(__file__).resolve().parents[1];root=pkg.parents[1];out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
flags=['-std=c17','-fshort-wchar','-g','-O1','-include','Uefi.h','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [pkg/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']]
src=[pkg/'Tests/ICBCrDxeTest.c',pkg/'Library/ICBLib/icb.c',pkg/'Target/Sm8450/CrInterconnectTarget.c',pkg/'Library/Sm8450InterconnectLib/Sm8450Interconnect.c']
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for s in src:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
inputs={str(x.relative_to(root)):sha(x) for x in sorted(deps)};exe=out/'DEBUG'
cmd=['clang',*flags,*map(str,src),'-Wl,--gc-sections','-o',str(exe)]
c=subprocess.run(cmd,text=True,capture_output=True);(out/'compile.log').write_text(c.stdout+c.stderr);assert c.returncode==0,c.stderr
r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);log=out/'test.log';log.write_text(r.stdout+r.stderr);assert r.returncode==0,r.stdout+r.stderr;print(r.stdout,end='')
assert inputs=={str(x.relative_to(root)):sha(x) for x in sorted(deps)},'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'PASS','scope':__doc__+'; actual native adapter/core and SM8450 graph, modeled DAL/CmdDB/RPMh/TPL; no hardware','inputs':inputs,'command':cmd,'test_log_sha256':sha(log),'executable_sha256':sha(exe)},indent=2)+'\n')
