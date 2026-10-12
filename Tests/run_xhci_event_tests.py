#!/usr/bin/env python3
"""Actual xHCI event-ring consumer and URB dispatch against independent producer."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
c=Path(__file__).resolve().parents[1];r=c.parents[1];d=r/'Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/XhciDxe'
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=d/'XhciSched.c');p.add_argument('--output',type=Path,required=True);p.add_argument('--expect-old-cycle',action='store_true');a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
s=a.source.read_text();functions=[]
for name in ('XhcSyncEventRing','XhcCheckNewEvent','IsTransferRingTrb','IsAsyncIntTrb','XhcCheckUrbResult','XhcEndpointToDci'):
 m=re.search(r'^(?:STATIC\n)?(?:EFI_STATUS|BOOLEAN|UINT8)\n(?:EFIAPI\n)?'+name+r' \([^;{]*\)\n\{',s,re.M);assert m,name
 end=m.end();depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 functions.append(s[m.start():end])
(a.output/'XhciEventProduction.c').write_text('\n\n'.join(functions)+'\n')
exe=a.output/'events';includes=[a.output,d,r/'MU_BASECORE/MdePkg/Include',r/'MU_BASECORE/MdePkg/Include/X64',r/'Platforms/CranePkg/Include',r/'MU_BASECORE/MdeModulePkg/Include']
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMDEPKG_NDEBUG','-fsanitize=address,undefined','-fno-sanitize-recover=all',*['-I'+str(x) for x in includes],str(c/'Tests/XhciEventRingTest.c'),'-o',str(exe)]
if a.expect_old_cycle:cmd.insert(1,'-DTEST_OLD_EVENT_CYCLE')
subprocess.run(cmd,check=True);run=subprocess.run([str(exe)],capture_output=True,text=True,check=True);print(run.stdout,end='')
inputs=[a.source,d/'XhciSched.h',d/'Xhci.h',c/'Tests/XhciEventRingTest.c',Path(__file__)]
(a.output/'artifacts.json').write_text(json.dumps({'scope':'actual-source event consumer ASan/UBSan; external DMA/NT producer modeled, no hardware root-cause claim','before':a.expect_old_cycle,'output':run.stdout,'command':cmd,'inputs':{str(x.resolve()):hashlib.sha256(x.read_bytes()).hexdigest() for x in inputs},'binary_sha256':hashlib.sha256(exe.read_bytes()).hexdigest()},indent=2)+'\n')
