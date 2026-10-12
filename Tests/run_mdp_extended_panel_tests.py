#!/usr/bin/env python3
"""Native XML parser and extended profile converter against real HDK assets."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-before',action='store_true');a=p.parse_args();o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg';d=c/'Driver/MDSSCrDxe'
s=[c/'Tests/MdpExtendedPanelTest.c',d/'MdpPanel.c',d/'MdpDsiSequence.c',d/'MdpExtendedPanel.c',c/'Library/MdssDsiCrLib/MdssDsiPanel.c',c/'Library/MdssDsiCrLib/MdssDsi.c',c/'Library/MdssDscCrLib/MdssDsc.c',c/'Library/MdssCrLib/Mdss.c',c/'Library/MdssCrLib/MdssHardware.c']
assets=r/'Platforms/WaipioCrPkg/Device/qcom-hdk8450/Binaries/RawFiles'
f=[assets/'Panel_lt9611uxc_dsi2hdmi_vid.xml',assets/'Panel_r66451_144hz_fhd_plus_dsc_vid_cphy.xml']
exe=o/'native';cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-ffunction-sections','-Wl,--gc-sections',*['-I'+str(x) for x in (c/'Include',r/'MU_BASECORE/MdePkg/Include',r/'MU_BASECORE/MdePkg/Include/X64')],*[str(x) for x in s],'-o',str(exe)]
if a.expect_before:cmd.insert(1,'-DCR_MDSS_PROFILE_BEFORE')
x=subprocess.run(cmd,text=True,capture_output=True);(o/'compile.log').write_text(x.stdout+x.stderr)
if x.returncode:print(x.stderr);x.check_returncode()
x=subprocess.run([str(exe),*[str(x) for x in f]],text=True,capture_output=True,timeout=60);(o/'test.log').write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='')
if a.expect_before:
 if x.returncode==0 or 'Raster.Soc==CR_MDSS_SOC_SM8350' not in x.stderr:raise RuntimeError('missing expected explicit SoC selection failure')
else:x.check_returncode()
inputs=s+f+[Path(__file__),d/'MdpPanel.h',d/'MdpExtendedPanel.h',c/'Include/Library/MdssDsiPanelCrLib.h']
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','expected_before_failure':a.expect_before,'scope':__doc__,'inputs':{str(x.relative_to(r)):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs}},indent=2)+'\n')
