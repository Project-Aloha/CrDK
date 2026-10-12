#!/usr/bin/env python3
"""Execute unchanged production RTC/TimeBaseLib and full FAT SetFileInfo."""
import argparse,hashlib,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True)
p.add_argument('--rtc-source',type=Path);p.add_argument('--before',action='store_true');p.add_argument('--cc',default='clang')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
crane=Path(__file__).resolve().parents[1];root=crane.parents[1];edk=root/'MU_BASECORE'
rtc=a.rtc_source or root/'Platforms/MUCommonCrPkg/Library/VirtualRealTimeClockLib/VirtualRealTimeClockLib.c'
calendar=edk/'EmbeddedPkg/Library/TimeBaseLib/TimeBaseLib.c';fat=edk/'FatPkg/EnhancedFatDxe'
misc=fat/'Misc.c';info=fat/'Info.c';fixture=crane/'Tests/VirtualRtcFatTest.c'
def extract(text,signature):
 start=text.index(signature);end=text.index('\n}\n',start)+3;return text[start:end]
chunks=[extract(misc.read_text(),s) for s in ('VOID\nFatEfiTimeToFatTime (','VOID\nFatFatTimeToEfiTime (','VOID\nFatGetCurrentFatTime (','BOOLEAN\nFatIsValidTime (')]
chunks+=[extract(info.read_text(),'EFI_STATUS\nFatSetFileInfo (')]
inc=a.output/'FatTimeActual.inc';inc.write_text('\n'.join(chunks))
exe=a.output/'virtual-rtc-fat'
cmd=[a.cc,'-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for d in (a.output,edk/'MdePkg/Include',edk/'MdePkg/Include/X64',edk/'MdeModulePkg/Include',edk/'EmbeddedPkg/Include',edk/'ArmPkg/Include',fat):cmd+=['-I'+str(d)]
subprocess.run(cmd+[str(fixture),str(rtc),str(calendar),'-o',str(exe)],check=True)
r=subprocess.run([str(exe)]+(['--before'] if a.before else []),check=True,capture_output=True,text=True);print(r.stdout,end='')
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
(a.output/'artifacts.json').write_text(json.dumps({'sources':{str(x):sha(x) for x in (rtc,calendar,misc,info,fixture,Path(__file__),inc)},'binary':sha(exe),'before':a.before,'output':r.stdout},indent=2)+'\n')
