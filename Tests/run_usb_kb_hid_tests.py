#!/usr/bin/env python3
"""Exercise production USB keyboard code and actual USB HID request builders."""
import argparse, hashlib, json, re, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--output',type=Path,required=True);p.add_argument('--source',type=Path);p.add_argument('--expect-idle-failure',action='store_true');a=p.parse_args()
root=Path(__file__).resolve().parents[3];edk=root/'MU_BASECORE';hid=root/'Platforms/CranePkg/Override/MU_PLUS/HidPkg';hid_headers=root/'Common/MU/HidPkg/Include';src=a.source or hid/'UsbKbHidDxe/UsbKbHidDxe.c';lib=edk/'MdePkg/Library/UefiUsbLib/Hid.c'
a.output.mkdir(parents=True,exist_ok=True)
def extract(s,n,optional=False):
 m=re.search(r'^(?:STATIC\n)?(?:EFI_STATUS|VOID|USB_KB_HID_DEV \*)\n(?:EFIAPI\n)?'+n+r' \(',s,re.M)
 if not m:
  if optional:return ''
  raise ValueError(n)
 return s[m.start():s.index('\n}\n',m.end())+3]
names=['UsbKbHidFind','UsbKbHidCleanup','UsbKbHidEnter','UsbKbHidLeave','SetOutputReport','RegisterKeyboardHidReportCallback','UnRegisterKeyboardHidReportCallback','InitUsbKeyboard','KeyboardHandler','UsbKbHidRecoveryHandler','UsbKbHidDriverBindingStop','UsbKbHidDriverBindingStart']
inc=a.output/'UsbKbActual.inc';inc.write_text('STATIC LIST_ENTRY mUsbKbHidDevices = INITIALIZE_LIST_HEAD_VARIABLE (mUsbKbHidDevices);\n'+'\n'.join([extract(lib.read_text(),n) for n in ['UsbSetReportRequest','UsbSetProtocolRequest','UsbGetProtocolRequest','UsbSetIdleRequest']]+[extract(src.read_text(),n,n in ('UsbKbHidEnter','UsbKbHidLeave','UsbKbHidFind','UsbKbHidCleanup')) for n in names]))
fixture=root/'Platforms/CranePkg/Tests/UsbKbHidTest.c';exe=a.output/'usb-kb-hid'
cmd=['clang','-std=c11','-fshort-wchar','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-O1','-fsanitize=address,undefined','-fno-sanitize-recover=all']
for d in [a.output,edk/'MdePkg/Include',edk/'MdePkg/Include/X64',edk/'MdeModulePkg/Include',hid_headers,hid/'UsbKbHidDxe']:
 cmd+=['-I'+str(d)]
subprocess.run(cmd+[str(fixture),'-o',str(exe)],check=True);r=subprocess.run([str(exe)],capture_output=True,text=True);print(r.stdout,end='');print(r.stderr,end='')
if a.expect_idle_failure:
 assert r.returncode != 0 and 'mIdles==1 && mProtocols==1 && mEvents==1 && mControls==2' in r.stderr, 'missing expected production SET_IDLE regression'
 assert 'ERROR: AddressSanitizer' not in r.stderr and 'runtime error:' not in r.stderr
else:r.check_returncode()
def sha(x):return hashlib.sha256(x.read_bytes()).hexdigest()
(a.output/'artifacts.json').write_text(json.dumps({'sources':{str(x):sha(x) for x in [src,lib,fixture,Path(__file__),hid/'UsbKbHidDxe/UsbKbHidDxe.h',inc]},'binary':sha(exe),'output':r.stdout,'stderr':r.stderr,'returncode':r.returncode,'expected_missing_idle':a.expect_idle_failure},indent=2)+'\n')
