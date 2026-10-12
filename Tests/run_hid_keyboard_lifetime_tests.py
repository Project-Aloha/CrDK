#!/usr/bin/env python3
"""Actual HID class lifecycle, queues and report/LED path with real EFI ABI."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1]
root=crane.parents[1]
default=crane/'Override/MU_PLUS/HidPkg/HidKeyboardDxe'
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--source-dir',type=Path,default=default)
p.add_argument('--case',default='all',choices=('all','queued','report','uninstall','uninstall-reentry'))
a=p.parse_args();directory=a.source_dir.resolve()
class_source=(directory/'HidKbDxe.c').read_text();core_source=(directory/'HidKeyboard.c').read_text()
fixed='ReleaseKeyboardDevice (' in class_source
class_names=('FindKeyboardDevice','HidKeyboardBeginCallback','HidKeyboardEndCallback','CloseKeyboardEvent','ReleaseKeyboardDevice',
 'HIDKeyboardDriverBindingStart','HIDKeyboardDriverBindingStop','HIDKeyboardReadKeyStrokeWorker','HIDKeyboardReset','HIDKeyboardResetEx',
 'HIDKeyboardReadKeyStroke','HIDKeyboardReadKeyStrokeEx','HIDKeyboardWaitForKey','HIDKeyboardSetState','KbdFreeNotifyList','IsKeyRegistered',
 'HIDKeyboardRegisterKeyNotify','HIDKeyboardUnregisterKeyNotify','KeyNotifyProcessHandler')
core_names=('GetKeyDescriptor','ReleaseKeyboardLayoutResources','InitHidKeyboard','HIDProcessKeyStrokesCallback','HidKeyboardRepeatHandler','ProcessKeyStroke',
 'HIDProcessModifierKey','InitQueue','DestroyQueue','IsQueueEmpty','IsQueueFull','Enqueue','Dequeue','SetKeyLED')
functions=[]
for source,names in ((class_source,class_names),(core_source,core_names)):
 for name in names:
  m=re.search(r'^(?:STATIC )?(?:EFI_STATUS|BOOLEAN|VOID|STATIC HID_KB_DEV \*|EFI_KEY_DESCRIPTOR \*)\n(?:EFIAPI\n)?'+name+r' \(',source,re.M)
  if not m:
   if name in class_names[:5] and not fixed:continue
   raise ValueError('No actual function '+name)
  body=source.index('{',m.end());depth=1;end=body+1
  while depth:depth+=(source[end]=='{')-(source[end]=='}');end+=1
  functions.append(source[m.start():end])
with tempfile.TemporaryDirectory(prefix='crane-hid-lifetime-') as tmp:
 out=Path(tmp);(out/'HidLifetimeProduction.c').write_text(('STATIC LIST_ENTRY mKeyboardDevices=INITIALIZE_LIST_HEAD_VARIABLE(mKeyboardDevices);\n' if fixed else '')+'\n\n'.join(functions))
 includes=[out,directory,default,root/'Common/MU/HidPkg/Include',root/'MU_BASECORE/MdePkg/Include',root/'MU_BASECORE/MdePkg/Include/X64',root/'MU_BASECORE/MdeModulePkg/Include']
 cmd=['clang','-std=c11','-fshort-wchar','-g','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-DMDEPKG_NDEBUG',
  '-fsanitize=address,undefined','-fno-sanitize-recover=all',*(['-DCR_HID_LIFETIME_FIXED'] if fixed else []),*['-I'+str(i) for i in includes],
  str(crane/'Tests/HidKeyboardLifetimeTest.c'),'-o',str(out/'test')]
 subprocess.run(cmd,check=True);subprocess.run([str(out/'test'),a.case],check=True,timeout=30)
