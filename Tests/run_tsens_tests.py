#!/usr/bin/env python3
"""Actual portable SM8450 TSENS reader under native and NT ASan/UBSan."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];edk=c.parents[1]/'MU_BASECORE'
sources=[c/'Tests/TsensCrTest.c',c/'Library/TsensCrLib/Tsens.c']
runs=[]
for nt in (False,True):
 exe=a.output/('tsens-nt' if nt else 'tsens-native');cmd=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
 if nt:cmd+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')]
 else:cmd+=['-fshort-wchar','-I'+str(edk/'MdePkg/Include'),'-I'+str(edk/'MdePkg/Include/X64')]
 cmd+=['-I'+str(c/'Include'),*map(str,sources),'-o',str(exe)]
 subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],check=True,capture_output=True,text=True);print(r.stdout,end='');runs.append({'nt':nt,'output':r.stdout,'sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
(a.output/'artifacts.json').write_text(json.dumps({'runs':runs,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in [*sources,c/'Include/Library/TsensCrLib.h',Path(__file__)]}},indent=2)+'\n')
