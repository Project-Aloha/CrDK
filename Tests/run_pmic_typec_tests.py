#!/usr/bin/env python3
"""Actual portable PAN policy/UCSI observer with independent raw packets."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);out=a.parse_args().output.resolve();out.mkdir(parents=True,exist_ok=True)
sources=[p/'Tests/PmicTypeCTest.c',p/'Library/PmicTypeCCrLib/PmicTypeC.c'];inputs=sources+[p/'Include/Library/PmicTypeCCrLib.h',p/'Include/ucsi/TypeC.h',Path(__file__)]
frozen={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs};runs={}
for name,kernel in [('efi',False),('nt',True)]:
 cmd=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
 if kernel:cmd+=['-D_KERNEL_MODE','-I'+str(p/'Tests/KernelMocks')]
 else:cmd+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 cmd+=['-I'+str(p/'Include'),*map(str,sources),'-o',str(out/name)]
 r=subprocess.run(cmd,capture_output=True,text=True);(out/(name+'-compile.log')).write_text(r.stdout+r.stderr);r.check_returncode()
 r=subprocess.run([str(out/name)],capture_output=True,text=True);(out/(name+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode();runs[name]={'command':cmd,'output':r.stdout}
assert frozen=={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
(out/'artifacts.json').write_text(json.dumps({'passed':True,'scope':__doc__,'inputs':frozen,'runs':runs},indent=2)+'\n')
