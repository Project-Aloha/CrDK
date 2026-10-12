#!/usr/bin/env python3
"""Actual xHCI Start/Stop and cleanup helpers against hardware/service lifetime assertions."""
from pathlib import Path
import argparse,hashlib,json,re,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
xhci=root/'Platforms/CranePkg/Override/MU_BASECORE/MdeModulePkg/Bus/Pci/XhciDxe'
a=argparse.ArgumentParser();a.add_argument('--output',type=Path,required=True)
a.add_argument('--source',type=Path,default=xhci/'Xhci.c');a.add_argument('--expect-before-failure',action='store_true')
a.add_argument('--expect-before-stop-failure',action='store_true')
a=a.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
source=a.source.read_text();parts=[]
for name in ['XhcFreeze','XhcExitBootService','XhcFindFailedStart','XhcCleanupFailedStart','XhcDriverBindingStart','XhcDriverBindingStop']:
 m=re.search(r'^(?:STATIC(?:\n| ))?(?:EFI_STATUS|VOID|USB_XHCI_INSTANCE \*)(?: EFIAPI)?\n(?:EFIAPI\n)?'+name+r' \([^;{]*\)\n+\{',source,re.M)
 if not m and a.expect_before_failure and name in ['XhcFindFailedStart','XhcCleanupFailedStart']:continue
 assert m,name
 end,depth=m.end(),1
 while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
 parts.append(source[m.start():end])
(out/'XhciStartProduction.c').write_text('\n\n'.join(parts))
cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMDEPKG_NDEBUG','-fsanitize=address,undefined','-fno-sanitize-recover=all']
if a.expect_before_failure:cmd+=['-DTEST_XHC_OLD_START']
if a.expect_before_stop_failure:cmd+=['-DTEST_XHC_OLD_STOP']
cmd+=['-I'+str(f) for f in [out,xhci,root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'Platforms/CranePkg/Include',root/'MU_BASECORE/MdeModulePkg/Include']]
cmd+=[str(p/'Tests/XhciStartTest.c'),'-o',str(out/'test')]
r=subprocess.run(cmd,capture_output=True,text=True);(out/'compile.log').write_text(r.stdout+r.stderr)
if r.returncode:print(r.stderr);r.check_returncode()
results=[]
for case in (['1','3','5','6'] if a.expect_before_failure else ['stop:8','stop:9','stop:10','stop:11'] if a.expect_before_stop_failure else [None]):
 r=subprocess.run([str(out/'test')]+([case] if case else []),capture_output=True,text=True)
 (out/('run'+(case or '')+'.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='')
 if a.expect_before_failure or a.expect_before_stop_failure:assert r.returncode!=0 and 'Assertion' in r.stderr
 else:r.check_returncode()
 results.append({'case':case,'returncode':r.returncode})
inputs=[a.source,xhci/'Xhci.h',p/'Tests/XhciStartTest.c',Path(__file__)]
(out/'artifacts.json').write_text(json.dumps({'passed':True,'expected_before_failure':a.expect_before_failure,'expected_before_stop_failure':a.expect_before_stop_failure,'command':cmd,'results':results,'inputs':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in inputs}},indent=2)+'\n')
