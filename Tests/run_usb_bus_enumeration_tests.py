#!/usr/bin/env python3
"""Run actual USB bus subtree removal and root/hub reconnect functions."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
root=Path(__file__).resolve().parents[3]
directory=root/'Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Usb/UsbBusDxe'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source',type=Path,default=directory/'UsbEnumer.c')
p.add_argument('--before',action='store_true')
p.add_argument('--output',type=Path,required=True)
a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
s=a.source.read_text();bodies=[]
hubsource=directory/'UsbHub.c'
for name in ('UsbRemoveDevice','UsbFindChild','UsbEnumeratePort','UsbHubEnumeration','UsbRootHubEnumeration','UsbOnHubInterrupt','UsbHubInit','UsbHubRelease'):
 selected=s if name not in ('UsbOnHubInterrupt','UsbHubInit','UsbHubRelease') else hubsource.read_text()
 m=re.search(r'^(?:EFI_STATUS|VOID|USB_DEVICE \*)\n(?:EFIAPI\n)?'+name+r' \([^;{]*\)\n\{',selected,re.M)
 assert m,name
 end=m.end();depth=1
 while depth:
  depth+=(selected[end]=='{')-(selected[end]=='}');end+=1
 bodies.append(selected[m.start():end])
(out/'UsbBusEnumerationProduction.c').write_text('\n\n'.join(bodies))
fixture=Path(__file__).with_name('UsbBusEnumerationTest.c');exe=out/'test'
flags=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMDEPKG_NDEBUG','-fsanitize=address,undefined','-fno-sanitize-recover=all']
if a.before:flags+=['-DTEST_BEFORE_RECONNECT_RETENTION']
for include in (out,directory,root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include'):flags+=['-I'+str(include)]
subprocess.run([*flags,str(fixture),'-o',str(exe)],check=True)
r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=30)
print(r.stdout,end='');print(r.stderr,end='');(out/'run.log').write_text(r.stdout+r.stderr);r.check_returncode()
inputs=[a.source,hubsource,directory/'UsbBus.h',fixture,Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'passed':True,'before':a.before,'sanitizers':['address','undefined'],'inputs':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs},'binary':hashlib.sha256(exe.read_bytes()).hexdigest()},indent=2)+'\n')
