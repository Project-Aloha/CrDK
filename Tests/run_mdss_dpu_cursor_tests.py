#!/usr/bin/env python3
"""Actual SM8450 cursor core with native/NT sanitizers; no hardware claim."""
from pathlib import Path
import argparse,hashlib,json,subprocess
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path,help='Actual saved cursor source for before-fix replay');a=p.parse_args()
r=Path(__file__).resolve().parents[3];c=r/'Platforms/CranePkg';o=a.output.resolve();o.mkdir(parents=True,exist_ok=True)
sources=[c/'Tests/MdssDpuCursorTest.c',a.source.resolve() if a.source else c/'Library/MdssDpu2dCrLib/MdssDpuCursor.c',c/'Library/MdssCrLib/Mdss.c']
commands=[]
for nt in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(c/'Include')]
 flags+=['-D_KERNEL_MODE','-I'+str(c/'Tests/KernelMocks')] if nt else ['-fshort-wchar','-I'+str(r/'MU_BASECORE/MdePkg/Include'),'-I'+str(r/'MU_BASECORE/MdePkg/Include/X64')]
 exe=o/('nt' if nt else 'native');cmd=flags+list(map(str,sources))+['-Wl,--gc-sections','-o',str(exe)];commands.append(cmd)
 x=subprocess.run(cmd,text=True,capture_output=True);(o/(exe.name+'-compile.log')).write_text(x.stdout+x.stderr)
 if x.returncode:print(x.stderr);x.check_returncode()
 x=subprocess.run([str(exe)],text=True,capture_output=True,timeout=45);(o/(exe.name+'.log')).write_text(x.stdout+x.stderr);print(x.stdout+x.stderr,end='');x.check_returncode()
files=sources+[c/'Include/Library/MdssDpuCursorCrLib.h',Path(__file__)]
refs=[r/'.vscode/linux/drivers/gpu/drm/msm/disp/dpu1'/n for n in ('catalog/dpu_8_1_sm8450.h','dpu_hw_sspp.c','dpu_hw_lm.c','dpu_hw_ctl.c','dpu_hw_vbif.c','dpu_hw_catalog.c','dpu_crtc.c','dpu_plane.c')]
refs += [r/'.vscode/android_kernel_modules_and_devicetree_oneplus_sm8450/vendor/qcom/opensource/display-drivers/msm/sde'/n for n in ('sde_hw_vbif.c','sde_vbif.c')]
(o/'artifacts.json').write_text(json.dumps({'status':'PASS','scope':'actual native/NT portable cursor core; independent controlled latches and callback faults; target Android upper halt status, no HAL/provider or hardware claim','commands':commands,'inputs':{str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in files},'references':{str(f.relative_to(r)):hashlib.sha256(f.read_bytes()).hexdigest() for f in refs}},indent=2)+'\n')
