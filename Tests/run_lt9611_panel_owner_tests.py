#!/usr/bin/env python3
"""Actual LT9611 panel-owner source under EFI GPIO/MCU lifecycle fault boundaries."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];r=c.parents[1];e=r/'MU_BASECORE'
sources=[c/'Tests/Lt9611PanelOwnerTest.c',c/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.c']
inputs=[*sources,Path(__file__),c/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.h',c/'Include/Protocol/EFILt9611PanelCrProtocol.h',c/'Include/Protocol/EFIGpioCrProtocol.h',c/'Include/Protocol/EFIMdpDsiProtocol.h']
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();frozen={str(f):sha(f) for f in inputs}
native=c/'Driver/MDSSCrDxe/MdpExtendedNative.c'
source=native.read_text()
def extract(name):
 # Select declaration directly; then retain the complete production body.
 start=source.rfind('STATIC ',0,source.index(name)+1)
 brace=source.index('{',start);depth=1;end=brace+1
 while depth:
  depth+=(source[end]=='{')-(source[end]=='}');end+=1
 return source[start:end]
(o/'ExtendedBlank.inc').write_text('\n'.join(extract(n) for n in ['Exact(', 'PanelReady(', 'PanelAcquire(', 'PanelRelease(', 'BlankOutput(']))
inputs += [native,native.with_suffix('.h'),c/'Driver/MDSSCrDxe/MdpNativeResources.h']
frozen={str(f):sha(f) for f in inputs}
cmd=['clang','-std=c17','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-DCRANE_HOST_TEST','-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(x) for x in (o,c/'Include',e/'MdePkg/Include',e/'MdePkg/Include/X64',e/'MdeModulePkg/Include')],*map(str,sources),'-o',str(o/'test')]
x=subprocess.run(cmd,text=True,capture_output=True);(o/'compile.log').write_text(x.stdout+x.stderr)
if x.returncode:print(x.stderr);x.check_returncode()
x=subprocess.run([str(o/'test')],text=True,capture_output=True,timeout=60);(o/'test.log').write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
assert frozen=={str(f):sha(f) for f in inputs},'source drift'
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'limits':'actual helper; EFI GPIO and resident MCU boundaries modeled, no physical electrical validation','inputs':frozen,'command':cmd},indent=2)+'\n')
