#!/usr/bin/env python3
"""DEBUG actual ICC producer/core console ownership regression; no hardware."""
from pathlib import Path
import argparse,hashlib,json,shlex,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path);p.add_argument('--expect-red',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[3];cr=root/'Platforms/CranePkg';out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
flags=['-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(cr/'Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
if a.source:flags+=['-DCR_ICB_DXE_SOURCE="'+str(a.source.resolve())+'"']
src=[cr/'Tests/IcbConsoleTest.c',cr/'Library/ICBLib/icb.c',cr/'Target/Sm8250/CrInterconnectTarget.c']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();deps={Path(__file__).resolve()}
for s in src:
 text=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True);deps.update(Path(x).resolve() for x in shlex.split(text.split(':',1)[1].replace('\\\n',' ')))
inputs={str(p.relative_to(root)):sha(p) for p in sorted(deps)}
cmd=['clang',*flags,*map(str,src),'-Wl,--gc-sections','-o',str(out/'DEBUG')];subprocess.run(cmd,check=True)
runs=[]
for mode in (['normal'] if a.expect_red else ['normal','bad-uart','generic','publish-error']):
 r=subprocess.run([str(out/'DEBUG'),mode],text=True,capture_output=True);log=out/(mode+'.log');log.write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
 if a.expect_red:assert r.returncode!=0 and 'Hardware[Q]!=0' in r.stderr
 else:r.check_returncode()
 runs.append({'mode':mode,'exit':r.returncode,'log':str(log.relative_to(root)),'sha256':sha(log)})
assert inputs=={str(p.relative_to(root)):sha(p) for p in sorted(deps)}
(out/'receipt.json').write_text(json.dumps({'status':'EXPECTED_RED' if a.expect_red else 'PASS','inputs':inputs,'command':cmd,'runs':runs,'binary_sha256':sha(out/'DEBUG'),'scope':'Actual native producer/core and SM8250 graph; modeled DAL/CmdDB/RPMh, DEBUG ASan/UBSan; no physical crash reproduction'},indent=2)+'\n')
