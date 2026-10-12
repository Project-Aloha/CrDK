#!/usr/bin/env python3
"""Native/NT sanitizer execution of actual GPIO owner bank and PEP GPIO core."""
from pathlib import Path
import subprocess
import tempfile
crane=Path(__file__).resolve().parents[1];workspace=crane.parents[1]
with tempfile.TemporaryDirectory(prefix='cr-gpio-owner-') as name:
 for kernel in (False,True):
  flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(crane/'Include')]
  if kernel: flags+=['-D_KERNEL_MODE','-pthread','-I'+str(crane/'Tests/KernelMocks')]
  else: flags+=['-fshort-wchar','-I'+str(workspace/'MU_BASECORE/MdePkg/Include'),'-I'+str(workspace/'MU_BASECORE/MdePkg/Include/X64')]
  exe=Path(name)/('nt' if kernel else 'native')
  subprocess.run([*flags,str(crane/'Tests/GpioOwnerTest.c'),str(crane/'Library/GpioOwnerLib/GpioOwner.c'),str(crane/'Library/PepCrLib/PepGpio.c'),'-o',str(exe)],check=True)
  subprocess.run([str(exe)],check=True,timeout=30)
