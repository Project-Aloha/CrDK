#!/usr/bin/env python3
"""Actual LT9611 panel-owner source under EFI GPIO/MCU lifecycle fault boundaries."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];r=c.parents[1];e=r/'MU_BASECORE'
sources=[c/'Tests/Lt9611PanelOwnerTest.c',c/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.c']
inputs=[*sources,Path(__file__),c/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.h',c/'Include/Protocol/EFILt9611PanelCrProtocol.h',c/'Include/Protocol/EFIGpioCrProtocol.h',c/'Include/Protocol/EFIMdpDsiProtocol.h']
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();frozen={str(f):sha(f) for f in inputs}
cmd=['clang','-std=c17','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-DCRANE_HOST_TEST','-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(x) for x in (c/'Include',e/'MdePkg/Include',e/'MdePkg/Include/X64')],*map(str,sources),'-o',str(o/'test')]
x=subprocess.run(cmd,text=True,capture_output=True);(o/'compile.log').write_text(x.stdout+x.stderr)
if x.returncode:print(x.stderr);x.check_returncode()
x=subprocess.run([str(o/'test')],text=True,capture_output=True,timeout=60);(o/'test.log').write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
assert frozen=={str(f):sha(f) for f in inputs},'source drift'
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'limits':'actual helper; EFI GPIO and resident MCU boundaries modeled, no physical electrical validation','inputs':frozen,'command':cmd},indent=2)+'\n')
