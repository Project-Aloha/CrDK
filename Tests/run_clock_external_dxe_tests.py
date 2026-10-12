#!/usr/bin/env python3
"""Actual native EFI external-clock wrappers + portable real lease/core/target."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True);o=a.parse_args().output.resolve();o.mkdir(parents=True,exist_ok=True)
sources=[p/'Tests/ClockExternalDxeTest.c',*[p/'Library/ClockExternalLib'/f for f in ('ClockExternal.c','ClockExternalPlan.c','ClockExternalLease.c')],p/'Library/ClockLib/fixed_pll.c',p/'Library/PepCrLib/PepJournal.c',p/'Library/PepCrLib/PepClock.c',p/'Driver/ClockCrDxe/GpuBootAccess.c']
inputs=sources+[Path(__file__),p/'Tests/ClockExternalLeaseTest.c',p/'Tests/ClockExternalTest.c',p/'Tests/ClockOwnershipTest.c',p/'Driver/ClockCrDxe/ClockCrDxe.c',p/'Target/Sm8450/CrClockTarget.c',p/'Library/Sm8450ClockLib/Sm8450Clock.c']+[p/'Library/ClockLib'/f for f in ('clock.c','clock_config.c','gdsc.c','clock_votes.c')]+[p/'Include/Library'/f for f in ('ClockExternalLib.h','ClockExternalPlan.h','ClockExternalLease.h')]+[p/'Include/Protocol'/f for f in ('EFIClockCrProtocol.h','EFIClockExternalCrProtocol.h')]
sha=lambda f:hashlib.sha256(f.read_bytes()).hexdigest();frozen={str(f.relative_to(root)):sha(f) for f in inputs}
exe=o/'clock-external-dxe';cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64'),*map(str,sources),'-o',str(exe)]
r=subprocess.run(cmd,capture_output=True,text=True);(o/'compile.log').write_text(r.stdout+r.stderr);print(r.stderr,end='');r.check_returncode()
r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=60);(o/'run.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
if frozen!={str(f.relative_to(root)):sha(f) for f in inputs}:raise RuntimeError('source drift')
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':frozen,'command':cmd,'binary_sha256':sha(exe)},indent=2)+'\n')
