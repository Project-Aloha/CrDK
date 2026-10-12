#!/usr/bin/env python3
"""Actual data-only MDSS wire core under native/NT ASan+UBSan."""
import argparse,hashlib,json,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[3];cr=root/'Platforms/CranePkg'
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
a.output=a.output.resolve();a.output.mkdir(parents=True,exist_ok=True)
source=cr/'Library/MdssHandoffCrLib/Handoff.c';fixture=cr/'Tests/MdssHandoffTest.c';rows=[]
for nt in (False,True):
    exe=a.output/('nt' if nt else 'native')
    cmd=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(cr/'Include')]
    if nt:
        original=cr/'Tests/KernelMocks/ntddk.h';text=original.read_text()
        definition='static inline VOID KeMemoryBarrier(VOID) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }'
        assert definition in text
        substitute=a.output/'ntddk-overlay.h';substitute.write_text(text.replace(definition,'VOID KeMemoryBarrier(VOID);'))
        overlay=a.output/'overlay.json';overlay.write_text(json.dumps({'version':0,'roots':[{'type':'file','name':str(original),'external-contents':str(substitute)}]}))
        cmd+=['-D_KERNEL_MODE','-ivfsoverlay',str(overlay),'-I'+str(cr/'Tests/KernelMocks')]
    else:cmd+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
    subprocess.run(cmd+[str(fixture),str(source),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],capture_output=True,text=True);print(r.stdout,end='');print(r.stderr,end='');r.check_returncode()
    rows.append({'mode':'NT' if nt else 'native','binary':str(exe),'sha256':hashlib.sha256(exe.read_bytes()).hexdigest(),'output':r.stdout})
inputs=[source,fixture,Path(__file__),cr/'Include/Interface/CrMdssHandoff.h',cr/'Include/Library/MdssHandoffCrLib.h']
(a.output/'artifacts.json').write_text(json.dumps({'scope':'actual pointer-free portable builder/parser; host native+NT ASan/UBSan, no hardware or OS grant claim','sources':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},'runs':rows},indent=2)+'\n')
