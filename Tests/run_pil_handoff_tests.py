#!/usr/bin/env python3
"""Compile actual native PIL publisher, ARC metadata and EFI checksum routines."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',required=True,type=Path);p.add_argument('--publisher',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];root=c.parents[1];edk=root/'MU_BASECORE'
sources=[c/'Tests/PilHandoffUefiTest.c',a.publisher or c/'Library/PILCrLib/PilHandoffUefi.c',c/'Library/RpmhArcVoteLib/RpmhArcVote.c',edk/'MdePkg/Library/BaseLib/CheckSum.c',edk/'EmbeddedPkg/EmbeddedMonotonicCounter/EmbeddedMonotonicCounter.c']
exe=a.output/'pil-handoff';cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for d in [c/'Include',edk/'MdePkg/Include',edk/'MdePkg/Include/X64']:cmd+=['-I'+str(d)]
objects=[]
for index, source in enumerate(sources):
 obj=a.output/('source-%u.o'%index);flags=['-Wno-unused-parameter'] if source.name=='EmbeddedMonotonicCounter.c' else []
 subprocess.run([v for v in cmd if v!='-Wl,--gc-sections']+flags+['-c',str(source),'-o',str(obj)],check=True);objects.append(obj)
subprocess.run(['clang','-fsanitize=address,undefined','-Wl,--gc-sections']+list(map(str,objects))+['-o',str(exe)],check=True)
r=subprocess.run([str(exe),str(a.output/'native-pilh.bin')],capture_output=True,text=True);print(r.stdout,end='');print(r.stderr,end='');r.check_returncode()
sha=lambda x:hashlib.sha256(x.read_bytes()).hexdigest()
(a.output/'artifacts.json').write_text(json.dumps({'sources':{str(x):sha(x) for x in sources+[Path(__file__),c/'Include/Library/PILCrHandoffUefiLib.h',c/'Include/Interface/CrPilHandoff.h']},'binary':sha(exe),'record':sha(a.output/'native-pilh.bin'),'output':r.stdout},indent=2)+'\n')
