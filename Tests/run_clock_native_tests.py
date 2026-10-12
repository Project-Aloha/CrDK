#!/usr/bin/env python3
"""Actual native/NT clock graph, handoff validation and retained firmware retirement."""
from pathlib import Path
import argparse,hashlib,json,subprocess,tempfile
p=Path(__file__).resolve().parents[1]
a=argparse.ArgumentParser(description=__doc__);a.add_argument('--output',type=Path);args=a.parse_args()
def run(out):
    out.mkdir(parents=True,exist_ok=True);runs=[]
    sources=[p/'Tests/ClockNativeTest.c',p/'Library/ClockExternalLib/ClockExternal.c', p/'Library/ClockExternalLib/ClockExternalPlan.c',p/'Library/MdssHandoffCrLib/Handoff.c',p/'Library/ClockLib/fixed_pll.c',p/'Library/PepCrLib/PepJournal.c',p/'Library/PepCrLib/PepClock.c']
    inputs=[*sources,Path(__file__),*[p/'Tests'/f for f in ('ClockExternalTest.c','ClockOwnershipTest.c','MdssHandoffTest.c')],*[p/'Library/ClockLib'/f for f in ('clock.c','gdsc.c','clock_config.c','clock_votes.c')],p/'Library/Sm8450ClockLib/Sm8450Clock.c',p/'Include/Library/ClockExternalLib.h',p/'Include/Interface/CrMdssHandoff.h',p/'Include/Library/MdssHandoffCrLib.h']
    frozen={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    for kernel in (False,True):
        mode='nt' if kernel else 'native';exe=out/mode
        cmd=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
        if kernel:cmd+=['-D_KERNEL_MODE','-pthread','-I'+str(p/'Tests/KernelMocks')]
        else:cmd+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),'-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
        cmd += [*[str(f) for f in sources],'-o',str(exe)]
        subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30)
        (out/(mode+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
        runs.append({'mode':mode,'command':cmd,'output':r.stdout,'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
    assert frozen=={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}
    (out/'artifacts.json').write_text(json.dumps({'scope':__doc__,'passed':True,'inputs':frozen,'runs':runs},indent=2)+'\n')
if args.output:run(args.output.resolve())
else:
    with tempfile.TemporaryDirectory(prefix='cr-clock-native-') as d:run(Path(d))
