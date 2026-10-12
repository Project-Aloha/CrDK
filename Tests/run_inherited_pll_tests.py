#!/usr/bin/env python3
"""Portable read-only direct PLL qualification against native/NT ABIs."""
from pathlib import Path
import argparse,subprocess
p=Path(__file__).resolve().parents[1];root=p.parents[1]
a=argparse.ArgumentParser();a.add_argument('--output',required=True,type=Path);o=a.parse_args().output.resolve();o.mkdir(parents=True,exist_ok=True)
for kernel in (False,True):
 flags=['clang','-std=c11','-g','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
  '-fno-sanitize-recover=all','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I'+str(p/'Include')]
 if kernel:flags+=['-D_KERNEL_MODE','-I'+str(p/'Tests/KernelMocks')]
 else:flags+=['-fshort-wchar','-I'+str(root/'MU_BASECORE/MdePkg/Include'),'-I'+str(root/'MU_BASECORE/MdePkg/Include/X64')]
 name='nt' if kernel else 'native';exe=o/name;cmd=flags+[str(p/'Tests/InheritedPllTest.c'),str(p/'Library/ClockLib/fixed_pll.c'),'-o',str(exe)]
 r=subprocess.run(cmd,text=True,capture_output=True);(o/(name+'-compile.log')).write_text(r.stdout+r.stderr);print(r.stderr,end='');r.check_returncode()
 r=subprocess.run([str(exe)],text=True,capture_output=True);(o/(name+'-run.log')).write_text(r.stdout+r.stderr);print(r.stdout+r.stderr,end='');r.check_returncode()
