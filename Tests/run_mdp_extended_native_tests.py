#!/usr/bin/env python3
"""Actual native extended frontend, private ClockLib service and display cores."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-before',action='store_true');a=p.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg'
s=[c/'Tests/MdpExtendedNativeTest.c',c/'Tests/MdpExtendedNativeClockFixture.c',c/'Driver/MDSSCrDxe/MdpExtendedNative.c',c/'Driver/MDSSCrDxe/MdpNativeResources.c',c/'Library/MdssCrLib/MdssRetire.c',
 *[c/f'Library/MdssDsiCrLib/{n}.c' for n in ('MdssDsi','MdssDsiCommands','MdssDsiClock','MdssDsiPanel','DsiPhyExtended')],
 c/'Library/MdssDualCrLib/MdssDual.c',c/'Library/MdssDscCrLib/MdssDsc.c',c/'Library/MdssCrLib/Mdss.c',c/'Library/MdssCrLib/MdssHardware.c',
 *[c/f'Library/ClockExternalLib/{n}.c' for n in ('ClockExternal','ClockExternalPlan','ClockExternalLease')],
 c/'Library/ClockLib/fixed_pll.c',c/'Library/PepCrLib/PepJournal.c',c/'Library/PepCrLib/PepClock.c']
exe=o/'native';cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-sign-compare',
 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections',
 *['-I'+str(x) for x in (c/'Include',r/'MU_BASECORE/MdePkg/Include',r/'MU_BASECORE/MdePkg/Include/X64',r/'MU_BASECORE/MdeModulePkg/Include')],*[str(x) for x in s],'-o',str(exe)]
x=subprocess.run(cmd,text=True,capture_output=True);(o/'compile.log').write_text(x.stdout+x.stderr)
if x.returncode:print(x.stderr);x.check_returncode()
x=subprocess.run([str(exe)],text=True,capture_output=True,timeout=90);(o/'test.log').write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='')
if a.expect_before:
 if x.returncode==0 or 'MdpExtendedNativeOpen' not in x.stderr:raise RuntimeError('missing expected target identity admission failure')
else:x.check_returncode()
inputs=s+[Path(__file__),c/'Driver/MDSSCrDxe/MdpExtendedNative.h',c/'Target/Sm8450/CrMdssTarget.c',c/'Include/Library/CrTargetMdssLib.h',c/'Tests/ClockExternalLeaseTest.c',c/'Tests/ClockExternalTest.c',c/'Tests/ClockOwnershipTest.c',c/'Tests/MdssDsiPanelTest.c',c/'Tests/MdssDsiExtendedTest.c',c/'Library/Sm8450ClockLib/Sm8450Clock.c']
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','expected_before_failure':a.expect_before,'scope':__doc__,'inputs':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
