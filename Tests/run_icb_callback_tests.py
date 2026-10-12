#!/usr/bin/env python3
"""DEBUG actual ICC/RPMh/QUP/I2C callback composition, modeled boundaries."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--source',type=Path)
p.add_argument('--expect-red',action='store_true')
a=p.parse_args();pkg=Path(__file__).resolve().parents[1];root=pkg.parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
flags=['-std=c17','-fshort-wchar','-g','-O1','-include','Uefi.h','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [pkg/'Tests/Mocks',pkg/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']]
if a.source:flags+=['-DCR_ICB_DXE_SOURCE="'+str(a.source.resolve())+'"']
src=[pkg/'Tests/IcbCallbackTest.c',pkg/'Library/ICBLib/icb.c',pkg/'Library/RpmhLib/rpmh.c',pkg/'Target/Sm8350/CrStorageInterconnectTarget.c']
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for s in src:
 raw=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True).replace('\\\n',' ')
 deps.update(Path(x).resolve() for x in shlex.split(raw.split(':',1)[1]))
inputs={str(x.relative_to(root)):sha(x) for x in sorted(deps)}
exe=out/'DEBUG';cmd=['clang',*flags,*map(str,src),'-Wl,--gc-sections','-o',str(exe)]
c=subprocess.run(cmd,text=True,capture_output=True);(out/'compile.log').write_text(c.stdout+c.stderr);assert c.returncode==0,c.stderr
runs=[]
for mode in (['application'] if a.expect_red else ['application','callback']):
 r=subprocess.run([str(exe),mode],text=True,capture_output=True,timeout=30);log=out/(mode+'.log');log.write_text(r.stdout+r.stderr)
 if a.expect_red:assert r.returncode!=0 and 'ICC callback reentered an owned spin lock' in r.stderr,r.stdout+r.stderr
 else:assert r.returncode==0,r.stdout+r.stderr
 print(r.stdout+r.stderr,end='');runs.append({'mode':mode,'exit':r.returncode,'log':str(log.relative_to(root)),'sha256':sha(log)})
assert inputs=={str(x.relative_to(root)):sha(x) for x in sorted(deps)},'source drift'
(out/'receipt.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_red else 'PASS','scope':__doc__+'; actual native adapter/core, real SM8350 topology, RPMh ISR, QUP and FIFO transfer; LT9611 worker scheduling body and provider/MMIO/IRQ boundaries modeled; no hardware stack claim','inputs':inputs,'command':cmd,'runs':runs,'executable_sha256':sha(exe)},indent=2)+'\n')
