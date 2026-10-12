#!/usr/bin/env python3
"""Actual native SD resources/OSKAL/core and GPIO frontend/pad failure tests."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import re
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--mode',choices=['DEBUG','RELEASE','ALL'],default='ALL')
a=p.parse_args()
cr=Path(__file__).resolve().parents[1]
root=cr.parents[1]
out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
inc=[cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/EmbeddedPkg/Include']
groups={
 'resources':[cr/'Tests/SdResourcesTest.c',cr/'Library/SdhciMsmLib/SdhciMsm.c'],
 'gpio':[cr/'Tests/GpioSdNativeTest.c',cr/'Library/GpioSdLib/GpioSd.c',cr/'Library/GpioPinLeaseLib/GpioPinLease.c',cr/'Library/GpioLib/gpio.c',cr/'Target/Sm8450/CrGpioTarget.c']}
runs=[]
frozen={}
def freeze(cmd,sources):
 opts=[x for x in cmd[:cmd.index(str(sources[0]))] if not x.startswith('-Wl,')]
 for src in sources:
  dep=subprocess.check_output(opts+['-MM',str(src)],text=True)
  for f in re.sub(r'\\\n',' ',dep).split(':',1)[1].split():
   path=Path(f).resolve();h=hashlib.sha256(path.read_bytes()).hexdigest()
   if str(path) in frozen and frozen[str(path)]!=h:raise RuntimeError('source drift '+str(path))
   frozen[str(path)]=h

for name,sources in groups.items():
 for debug in ([True] if a.mode=='DEBUG' else [False] if a.mode=='RELEASE' else [True,False]):
  mode=name+('-debug' if debug else '-release');exe=out/mode
  cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fsanitize=address,undefined','-fno-sanitize-recover=all','-include','Uefi.h']
  if not debug:cmd+=['-DMDEPKG_NDEBUG']
  cmd+=['-I'+str(i) for i in inc]+[str(s) for s in sources]+['-o',str(exe)]
  freeze(cmd,sources)
  subprocess.run(cmd,check=True)
  r=subprocess.run([str(exe)],text=True,capture_output=True);print(r.stdout+r.stderr,end='');r.check_returncode()
  (out/(mode+'.log')).write_text(r.stdout+r.stderr)
  runs.append({'mode':mode,'command':cmd,'stdout':r.stdout,'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()})
inputs=[Path(__file__),cr/'Driver/SdhciCrDxe/SdResources.c',cr/'Driver/SdhciCrDxe/SdResources.h',cr/'Driver/GpioCrDxe/GpioCrDxe.c',cr/'Include/Protocol/EFIGpioCrProtocol.h',cr/'Library/OskInterruptLib/oskinterrupt.c',cr/'Include/Library/GpioSdLib.h']
for s in groups.values():inputs+=s
arm_sources=[cr/'Driver/SdhciCrDxe/SdResources.c',cr/'Driver/GpioCrDxe/GpioCrDxe.c',cr/'Library/GpioSdLib/GpioSd.c',cr/'Library/SdhciMsmLib/SdhciMsm.c',cr/'Driver/SdhciCrDxe/MmcHost.c',cr/'Driver/SdhciCrDxe/SdhciCrDxe.c',cr/'Library/CrDalLib/CrDalLib.c',cr/'Target/Sm8450/CrSdTarget.c']
arm_inc=[cr/'Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/AArch64',root/'MU_BASECORE/EmbeddedPkg/Include']
for s in arm_sources:
 cmd=['clang','--target=aarch64-none-elf','-std=c11','-ffreestanding','-fshort-wchar','-fno-stack-protector','-Wall','-Wextra','-Werror','-Wno-unused-parameter']+['-I'+str(i) for i in arm_inc]+['-c',str(s),'-o',str(out/(s.stem+'-arm64.o'))]
 freeze(cmd,[s])
 subprocess.run(cmd,check=True);runs.append({'mode':'ARM64-compile','command':cmd,'object_sha256':hashlib.sha256((out/(s.stem+'-arm64.o')).read_bytes()).hexdigest()})
inputs+=arm_sources
for f,h in frozen.items():
 if hashlib.sha256(Path(f).read_bytes()).hexdigest()!=h:raise RuntimeError('source drift '+f)
(out/'artifacts.json').write_text(json.dumps({'scope':'Actual native production engines; modeled fallible EFI/provider/MMIO. No physical SD-card test.','inputs':frozen,'sources':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},'runs':runs},indent=2)+'\n')
