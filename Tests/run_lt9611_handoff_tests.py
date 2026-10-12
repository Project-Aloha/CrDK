#!/usr/bin/env python3
"""Actual native LT9611 adapter with HDK participant and disabled-board tests."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess

root=Path(__file__).resolve().parents[3]
cr=root/'Platforms/CranePkg'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--cc',default='clang')
a=p.parse_args()
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixture=cr/'Tests/Lt9611I2cTest.c'
exe=out/'lt9611-handoff'
command=[a.cc,'-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror',
    '-DCRANE_HOST_TEST','-fsanitize=address,undefined','-fno-sanitize-recover=all',
    '-I'+str(cr/'Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include'),
    '-I'+str(root/'MU_BASECORE/MdePkg/Include/X64'),str(fixture),'-o',str(exe)]
subprocess.run(command,check=True)
run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
print(run.stdout,end='');print(run.stderr,end='');run.check_returncode()
files=[fixture,cr/'Driver/Lt9611uxcCrDxe/lt9611.c',cr/'Driver/Lt9611uxcCrDxe/lt9611.inf',
    cr/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.c',cr/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.h',
    cr/'Include/Protocol/EFILt9611PanelCrProtocol.h',
    cr/'Library/MdssHandoffCrLib/Handoff.c',cr/'Include/Protocol/EFIMdssHandoffProtocol.h',
    cr/'Include/Interface/CrMdssHandoff.h',Path(__file__)]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
(out/'artifacts.json').write_text(json.dumps({'scope':
    'actual LT9611 native source and portable wire Append under ASan/UBSan; GPIO/MCU/boot-service boundaries controlled by fixture; no actual hardware handoff claim',
    'command':command,'output':run.stdout,'inputs':{str(f):sha(f) for f in files},
    'binary_sha256':sha(exe)},indent=2)+'\n')
