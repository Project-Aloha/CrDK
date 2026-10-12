#!/usr/bin/env python3
"""Actual optional audio-PD QRTR parser/queue and shared GLINK composition."""
import argparse, hashlib, json, shlex, subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',required=True,type=Path)
a=p.parse_args();c=Path(__file__).resolve().parents[1];r=c.parents[1];o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
(o/'artifacts.json').unlink(missing_ok=True)
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
for name in ('Qrtr','Glink','PmicGlink','ChargerLog'):(o/(name+'.tmh')).write_text('/* Host WPP shape shim, not actual WDK. */\n')
inputs={Path(__file__).resolve(),c/'Include/ucsi/Qrtr.h'};runs=[]
qrtr=c/'Library/UcsiCrLib/Qrtr.c';glink=c/'Library/UcsiCrLib/Glink.c';gpr=c/'Library/AudioGprCrLib/AudioGpr.c'
programs=[('audio-qrtr',[c/'Tests/QrtrAudioTest.c',qrtr]),('audio-glink',[c/'Tests/QrtrAudioGlinkTest.c',qrtr,glink,gpr])]
legacy=[('legacy-qrtr',[c/'Tests/QrtrServiceTest.c',qrtr]),('legacy-lifecycle',[c/'Tests/UcsiDxeLifecycleTest.c',qrtr,c/'Library/UcsiCrLib/PmicGlink.c',c/'Library/UcsiCrLib/ChargerLog.c'])]
for nt in (False,True):
    flags=['-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    if nt:flags+=['-D_KERNEL_MODE','-pthread','-I'+str(c/'Tests/KernelMocks'),'-I'+str(o)]
    else:flags+=['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64'),'-I'+str(r/'MU_BASECORE/EmbeddedPkg/Include')]
    flags+=['-I'+str(c/'Include')]
    selected=programs if nt else programs+legacy
    for _,sources in selected:
        for s in sources:
            dep=subprocess.check_output(['clang',*flags,'-MM',str(s)],text=True)
            inputs.update(Path(x).resolve() for x in shlex.split(dep.replace('\\\n','').split(':',1)[1]))
    before={str(f):sha(f) for f in sorted(inputs)}
    if runs:assert all(before[k]==v for k,v in runs[0]['inputs'].items()),'input changed between ABIs'
    for name,sources in selected:
        exe=o/(name+('-nt' if nt else '-native'));cmd=['clang',*flags,*map(str,sources),'-o',str(exe)]
        x=subprocess.run(cmd,text=True,capture_output=True);(o/(exe.name+'-compile.log')).write_text(x.stdout+x.stderr)
        if x.returncode:print(x.stdout+x.stderr);x.check_returncode()
        x=subprocess.run([str(exe)],text=True,capture_output=True,timeout=45);(o/(exe.name+'.log')).write_text(x.stdout+x.stderr)
        print(x.stdout+x.stderr,end='');x.check_returncode()
        runs.append({'nt':nt,'command':cmd,'run':[str(exe)],'binary_sha256':sha(exe),'output':x.stdout,'inputs':before})
    assert before=={str(f):sha(f) for f in sorted(inputs)},'compiled input changed'
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'Actual QRTR/GLINK cores with synthetic peer; optional audio listener. Contrasts post-DOWN old payload with and without local callback quiesce; no remote graph/DMA retirement, Windows activation or physical audio claim.','runs':runs,'inputs':{str(f):sha(f) for f in sorted(inputs)}},indent=2)+'\n')
