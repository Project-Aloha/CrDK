#!/usr/bin/env python3
"""Actual portable HID button-state core under native and NT ASAN/UBSAN."""
from pathlib import Path
import subprocess,tempfile
p=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='buttons-input-') as d:
 for kernel in (False,True):
  flags=['clang','-std=c17','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(p/'Include')]
  if kernel:flags+=['-D_KERNEL_MODE','-I'+str(p/'Tests/KernelMocks')]
  else:flags+=['-fshort-wchar','-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include'),'-I'+str(p.parents[1]/'MU_BASECORE/MdePkg/Include/X64')]
  binary=Path(d)/('nt' if kernel else 'native')
  subprocess.run(flags+[str(p/'Tests/ButtonsInputTest.c'),str(p/'Library/ButtonsInputLib/ButtonsInput.c'),'-o',str(binary)],check=True)
  subprocess.run([str(binary)],check=True,timeout=30)
