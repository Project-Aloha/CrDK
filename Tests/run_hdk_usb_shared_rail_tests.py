#!/usr/bin/env python3
"""Execute actual HDK8350 USB/PCIe rail contracts through the native RPMh DXE."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
a=p.parse_args()
cr=Path(__file__).resolve().parents[1];root=cr.parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
wrapper=out/'pcie-target.c'
wrapper.write_text('#define CrTargetGetPcieContext CrHdk8350GetPcieContext\n#include "'+str(cr/'Target/Sm8350/CrPcieTarget.c')+'"\n')
stubs=out/'debug-stubs.c'
stubs.write_text('#include <Uefi.h>\n#include <assert.h>\nBOOLEAN EFIAPI DebugAssertEnabled(VOID){return TRUE;}\nVOID EFIAPI DebugAssert(CONST CHAR8 *F,UINTN L,CONST CHAR8 *D){(VOID)F;(VOID)L;(VOID)D;assert(0);}\n')
flags=['-std=c17','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror',
       '-DCR_HDK_USB_SHARED_RAIL_TEST','-fsanitize=address,undefined','-fno-sanitize-recover=all',
       '-ffunction-sections','-fdata-sections']
flags+=['-I'+str(x) for x in [cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']]
sources=[cr/x for x in ['Tests/RpmhCrDxeTest.c','Target/Sm8350/CrSdPowerTarget.c','Library/RpmhLib/rpmh_rail.c','Library/RpmhArcVoteLib/RpmhArcVote.c']]+[wrapper,stubs]
deps={Path(__file__).resolve()}
for src in sources:
 text=subprocess.check_output(['clang',*flags,'-MM','-MT','test',str(src)],text=True)
 deps.update(Path(x).resolve() for x in shlex.split(text.split(':',1)[1].replace('\\\n',' ')))
sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
inputs={str(x.relative_to(root) if x.is_relative_to(root) else x):sha(x) for x in sorted(deps)}
exe=out/'DEBUG';command=['clang',*flags,*map(str,sources),'-Wl,--gc-sections','-o',str(exe)]
subprocess.run(command,check=True)
r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30)
(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
r.check_returncode()
assert inputs=={str(x.relative_to(root) if x.is_relative_to(root) else x):sha(x) for x in sorted(deps)}
(out/'receipt.json').write_text(json.dumps({'status':'PASS','mode':'DEBUG host ASan/UBSan','scope':'actual target supplies + native RPMh DXE/rail engine; modeled CmdDB/TCS; no hardware proof',
 'inputs':inputs,'command':command,'exit':r.returncode,'executable_sha256':sha(exe),'log_sha256':sha(out/'test.log')},indent=2)+'\n')
