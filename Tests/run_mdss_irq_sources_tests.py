#!/usr/bin/env python3
"""Actual SM8450 IRQ source qualifier, native/NT ASan/UBSan boundaries."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg';o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
(o/'artifacts.json').unlink(missing_ok=True)
sources=[c/'Tests/MdssIrqSourcesTest.c',c/'Library/MdssDpuInterruptCrLib/MdssIrqSources.c']
commands=[]
for nt in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
 exe=o/('nt' if nt else 'native');cmd=flags+list(map(str,sources))+['-o',str(exe)];commands.append(cmd)
 x=subprocess.run(cmd,text=True,capture_output=True);(o/(exe.name+'-compile.log')).write_text(x.stdout+x.stderr)
 if x.returncode:print(x.stderr);x.check_returncode()
 x=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30);(o/(exe.name+'.log')).write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
files=sources+[c/'Include/Library/MdssDpuInterruptCrLib.h',c/'Include/Library/MdssIrqSourcesCrLib.h',Path(__file__)]
linux=r/'.vscode/linux';android=r/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450'
refs=[linux/'drivers/gpu/drm/msm'/n for n in ('disp/dpu1/dpu_hw_interrupts.c','disp/dpu1/catalog/dpu_8_1_sm8450.h','registers/display/dsi.xml','dsi/dsi_cfg.h','dp/dp_reg.h')]
refs+=[android/'vendor/qcom/opensource/display-drivers/msm/dp'/n for n in ('dp_catalog.h','dp_catalog.c','dp_hdcp2p2.c')]
refs+=[android/'vendor/qcom/opensource/display-drivers/msm/dsi'/n for n in ('dsi_ctrl_hw_cmn.c','dsi_catalog.c','dsi_ctrl_reg.h')]
refs+=[android/'vendor/qcom/opensource/display-drivers/msm/sde_hdcp_1x.c']
refs+=[android/'vendor/qcom/opensource/display-drivers/msm/sde'/n for n in ('sde_hw_catalog.c','sde_hw_interrupts.c')]
dt_commit='877203a7cf9b79973d44a9db716490f95d820110'
dt_blobs={}
for name in ('waipio-sde.dtsi','waipio-sde-common.dtsi','waipio-sde-display-hdk.dtsi'):
 path='vendor/qcom/proprietary/display-devicetree/display/'+name
 blob=subprocess.check_output(['git','-C',str(android),'show',dt_commit+':'+path])
 dt_blobs[dt_commit+':'+path]=hashlib.sha256(blob).hexdigest()
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'actual read-only core native/NT sanitizer fixture; no hardware, resource grant or IRQ connection claim','commands':commands,'inputs':{str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},'references':{str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in refs},'target_dt_blobs':dt_blobs,'reference_commits':{str(x.relative_to(r)):subprocess.check_output(['git','-C',str(x),'rev-parse','HEAD'],text=True).strip() for x in (linux,android)}},indent=2)+'\n')
