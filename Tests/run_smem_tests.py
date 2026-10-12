#!/usr/bin/env python3
"""Compile the production SMEM reader with synthetic fixtures and sanitizers."""
from pathlib import Path
import argparse
import subprocess
import tempfile
crane = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser()
p.add_argument('--cc', default='clang')
p.add_argument('--edk2', type=Path, default=crane.parents[1] / 'MU_BASECORE')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='smem-tests-') as tmp:
    exe = Path(tmp) / 'test'
    subprocess.run([a.cc, '-std=c11', '-fshort-wchar', '-Wall', '-Wextra',
                    '-Werror', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I'+str(crane/'Include'),
                    '-I'+str(a.edk2/'MdePkg/Include'),
                    '-I'+str(a.edk2/'MdePkg/Include/X64'),
                    str(crane/'Library/SmemLib/smem.c'),
                    str(crane/'Tests/SmemLibTest.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
