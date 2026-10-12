#!/usr/bin/env python3
"""Actual battery DXE and portable core against EFI service boundary faults."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
c=Path(__file__).resolve().parents[1];root=c.parents[1]
sources=[c/'Tests/BatteryChargerDxeTest.c',c/'Library/BatteryChargerCrLib/BatteryCharger.c']
inputs=[*sources,c/'Driver/BatteryChargerCrDxe/BatteryChargerCrDxe.c',c/'Include/Library/BatteryChargerCrLib.h',c/'Include/Protocol/EFIBatteryChargerCrProtocol.h',c/'Include/Protocol/EFIPmicGlinkCrProtocol.h',Path(__file__)]
exe=a.output/'battery-dxe'
cmd=['clang','-std=c17','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(x) for x in [c/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64']],*map(str,sources),'-o',str(exe)]
subprocess.run(cmd,check=True);r=subprocess.run([str(exe)],check=True,capture_output=True,text=True);print(r.stdout,end='')
(a.output/'artifacts.json').write_text(json.dumps({'result':'PASS','output':r.stdout,'inputs':{str(x):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},'binary':hashlib.sha256(exe.read_bytes()).hexdigest()},indent=2)+'\n')
