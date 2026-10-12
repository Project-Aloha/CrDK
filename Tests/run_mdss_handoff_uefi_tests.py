#!/usr/bin/env python3
"""Actual native driver/storage coordinator + portable core, with optional before inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
modes=('publication','warnings','overlap','interrupted','entry','registration','cache-map','mapping','deferred','release','page-extents','mapping-extent')
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--uefi-source',type=Path)
p.add_argument('--driver-source',type=Path)
p.add_argument('--mode',choices=modes,action='append',help='Run only these modes; default runs all')
a=p.parse_args();cr=Path(__file__).resolve().parents[1];root=cr.parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
fixture=cr/'Tests/MdssHandoffUefiTest.c';core=cr/'Library/MdssHandoffCrLib/Handoff.c'
uefi=(a.uefi_source or cr/'Library/MdssHandoffUefiLib/MdssHandoffUefi.c').resolve()
driver=(a.driver_source or cr/'Driver/MdssHandoffCrDxe/MdssHandoffCrDxe.c').resolve()
includes=[cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']
exe=out/'native-coordinator'
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections',
     '-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(i) for i in includes],
     '-DCR_MDSS_UEFI_SOURCE="'+str(uefi)+'"','-DCR_MDSS_DRIVER_SOURCE="'+str(driver)+'"',str(fixture),str(core),'-o',str(exe)]
subprocess.run(cmd,check=True);rows=[]
dependencies=set()
for source in (fixture,core):
    depcmd=['clang','-std=c11','-fshort-wchar',*['-I'+str(i) for i in includes],
        '-DCR_MDSS_UEFI_SOURCE="'+str(uefi)+'"','-DCR_MDSS_DRIVER_SOURCE="'+str(driver)+'"','-MM','-MT','inputs',str(source)]
    deps=subprocess.run(depcmd,check=True,capture_output=True,text=True).stdout
    (out/(source.stem+'.d')).write_text(deps)
    dependencies.update(Path(i).resolve() for i in shlex.split(deps.replace('\\\n',' ').split(':',1)[1]))
for mode in a.mode or modes:
    run=[str(exe),mode]
    if mode=='publication':run+=[str(out/'record.bin'),str(out/'locator.bin')]
    r=subprocess.run(run,capture_output=True,text=True);(out/(mode+'.log')).write_text(r.stdout+r.stderr)
    print(r.stdout+r.stderr,end='');rows.append({'mode':mode,'returncode':r.returncode})
inputs=[fixture,cr/'Tests/MdssHandoffTest.c',core,uefi,driver,cr/'Include/Library/MdssHandoffUefiLib.h',cr/'Include/Interface/CrMdssHandoff.h',Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'scope':'actual native storage/driver/core; host memory/boot-services/SMMU producer models; wire cross-test, no OS or hardware grant',
    'command':cmd,'sources':{str(i):hashlib.sha256(i.read_bytes()).hexdigest() for i in inputs},
    'compiled_inputs':{str(i):hashlib.sha256(i.read_bytes()).hexdigest() for i in sorted(dependencies)},
    'outputs':{str(i):hashlib.sha256(i.read_bytes()).hexdigest() for i in (exe,out/'record.bin',out/'locator.bin') if i.exists()},'runs':rows},indent=2)+'\n')
raise SystemExit(0 if all(r['returncode']==0 for r in rows) else 1)
