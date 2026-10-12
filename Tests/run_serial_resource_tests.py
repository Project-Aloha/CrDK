#!/usr/bin/env python3
"""Native/NT actual portable serial resource startup/retirement faults."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];root=c.parents[1];inputs=[c/'Tests/SerialResourceTest.c',c/'Library/SerialResourceLib/SerialResources.c',c/'Include/Library/SerialResourceLib.h']
(o/'artifacts.json').unlink(missing_ok=True)
for nt in (False,True):
 b=o/('nt' if nt else 'native');f=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
 f+=['-D_KERNEL_MODE','-pthread','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 subprocess.run(f+[str(x) for x in inputs[:2]]+['-o',str(b)],check=True);subprocess.run([str(b)],check=True)
(o/'artifacts.json').write_text(json.dumps({'scope':__doc__,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
