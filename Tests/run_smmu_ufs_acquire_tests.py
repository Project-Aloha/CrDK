#!/usr/bin/env python3
"""DEBUG real-core UFS inherited-route regression under ASan/UBSan."""
from pathlib import Path
import argparse
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--cc', default='clang')
parser.add_argument('--core', type=Path)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
crane = Path(__file__).resolve().parents[1]
edk = crane.parents[1] / 'MU_BASECORE'
with tempfile.TemporaryDirectory(prefix='cr-ufs-acquire-') as tmp:
    out = args.out or Path(tmp)
    out.mkdir(parents=True, exist_ok=True)
    exe = out / 'debug'
    command = [args.cc, '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall',
               '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-I'+str(crane/'Include'),
               '-I'+str(edk/'MdePkg/Include'), '-I'+str(edk/'MdePkg/Include/X64'),
               str(args.core or crane/'Library/SmmuLib/smmu.c'),
               str(crane/'Tests/SmmuUfsAcquireTest.c'), '-o', str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)
    composed = out / 'debug-dxe'
    command = [args.cc, '-std=c11', '-fshort-wchar', '-g', '-O1', '-Wall',
               '-Wextra', '-Werror', '-fsanitize=address,undefined',
               '-fno-sanitize-recover=all', '-ffunction-sections', '-fdata-sections',
               '-Wl,--gc-sections', '-I'+str(crane/'Include'),
               '-I'+str(edk/'MdePkg/Include'), '-I'+str(edk/'MdePkg/Include/X64'),
               '-I'+str(edk/'MdeModulePkg/Include'),
               str(args.core or crane/'Library/SmmuLib/smmu.c'),
               str(crane/'Library/SmmuLib/SmmuDisplay.c'),
               str(crane/'Library/SmmuLib/SmmuDisplayOwnership.c'),
               str(crane/'Tests/SmmuUfsDxeAcquireTest.c'), '-o', str(composed)]
    subprocess.run(command, check=True)
    subprocess.run([str(composed)], check=True)
