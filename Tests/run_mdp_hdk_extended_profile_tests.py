#!/usr/bin/env python3
"""Actual packaged HDK profile producer, bridge owner, native entry/GOP and display/clock cores."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);o=p.parse_args().output.resolve();o.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg'
s=[c/'Tests/MdpHdkExtendedProfileTest.c',c/'Tests/MdpExtendedNativeClockFixture.c',
 *[c/f'Driver/MDSSCrDxe/{n}.c' for n in ('MdpExtendedNative','MdpExtendedOutput','MdpExtendedPanel','MdpExtendedEntry','MdpPanel','MdpDsiSequence')],
 *[c/f'Library/MdssDsiCrLib/{n}.c' for n in ('MdssDsi','MdssDsiCommands','MdssDsiClock','MdssDsiPanel','DsiPhyExtended')],
 c/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.c',c/'Library/MdssDualCrLib/MdssDual.c',c/'Library/MdssDscCrLib/MdssDsc.c',c/'Library/MdssCrLib/Mdss.c',c/'Library/MdssCrLib/MdssHardware.c',c/'Library/MdssCrLib/MdssRetire.c',c/'Driver/MDSSCrDxe/MdpNativeResources.c',
 *[c/f'Library/ClockExternalLib/{n}.c' for n in ('ClockExternal','ClockExternalPlan','ClockExternalLease')],
 c/'Library/ClockLib/fixed_pll.c',c/'Library/PepCrLib/PepJournal.c',c/'Library/PepCrLib/PepClock.c',
 r/'MU_BASECORE/MdeModulePkg/Library/FrameBufferBltLib/FrameBufferBltLib.c',c/'Library/MdssHandoffCrLib/Handoff.c']
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-sign-compare',
 '-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections',
 *['-I'+str(x) for x in (c/'Include',r/'Platforms/MUCommonCrPkg/Include',r/'MU_BASECORE/MdePkg/Include',
 r/'MU_BASECORE/MdePkg/Include/X64',r/'MU_BASECORE/MdeModulePkg/Include')],*[str(x) for x in s],'-o',str(o/'native')]
x=subprocess.run(cmd,text=True,capture_output=True);(o/'compile.log').write_text(x.stdout+x.stderr)
if x.returncode:print(x.stderr);x.check_returncode()
x=subprocess.run([str(o/'native'),str(r/'Platforms/WaipioCrPkg/Device/qcom-hdk8450/Binaries/RawFiles/Panel_lt9611uxc_dsi2hdmi_vid.xml')],text=True,capture_output=True,timeout=180);(o/'test.log').write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
inputs=s+[Path(__file__),c/'Tests/MdpExtendedOutputTest.c',c/'Driver/MDSSCrDxe/MdpHdkExtendedProfile.c',c/'Driver/MDSSCrDxe/MdpHdkExtendedProfile.h',c/'Driver/Lt9611uxcCrDxe/Lt9611PanelOwner.h',r/'Platforms/WaipioCrPkg/Device/qcom-hdk8450/Binaries/RawFiles/Panel_lt9611uxc_dsi2hdmi_vid.xml',c/'Tests/MdpExtendedNativeTest.c',c/'Tests/MdssDsiPanelTest.c',c/'Tests/MdssDsiExtendedTest.c',
 *[c/f'Driver/MDSSCrDxe/{n}.h' for n in ('MdpExtendedNative','MdpExtendedOutput','MdpExtendedPanel','MdpExtendedEntry','MdpPanel','MdpDsiSequence')],
 *[c/f'Tests/{n}.c' for n in ('ClockExternalLeaseTest','ClockExternalTest','ClockOwnershipTest')],c/'Library/Sm8450ClockLib/Sm8450Clock.c']
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':__doc__,'inputs':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
